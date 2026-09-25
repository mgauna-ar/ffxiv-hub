#include "app/ui/combat_timeline.hpp"
#include "app/app_state.hpp"
#include "app/ui/combat_view_common.hpp"
#include "app/ui/theme.hpp"
#include "app/ui/widgets.hpp"
#include "common/ui/icons.hpp"
#include "meter/timeline.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <string>
#include <unordered_set>
#include <vector>

namespace hub::app::ui {

#ifdef HAVE_IMGUI
namespace {

/// A live pull is fetched again this often while the tab is open. An archived one
/// never changes, so it is fetched once.
constexpr auto kLiveRefresh = std::chrono::milliseconds(250);

meter::TimelineMetric s_metric = meter::TimelineMetric::Damage;
size_t s_smoothing_s = 15;
/// Players whose line the legend turned off, kept from one pull to the next.
std::unordered_set<meter::EntityId> s_hidden;

struct Fetched {
    bool valid{false};
    uint64_t encounter_id{0};
    std::chrono::steady_clock::time_point at{};
    uint64_t version{0};
    meter::EncounterTimeline timeline;
};
Fetched s_fetched;

/// One player's line. Worked out again only when the data, a setting or the chart's
/// width changes, never per frame.
struct Line {
    meter::EntityId entity{0};
    std::string name;
    uint32_t color{0};
    double total{0.0};           // Orders the legend
    float peak{0.0f};
    std::vector<float> values;   // One per point across the chart
};

struct Chart {
    uint64_t version{0};
    meter::TimelineMetric metric{meter::TimelineMetric::Damage};
    meter::DpsMetric dps{meter::DpsMetric::Dps};
    size_t smoothing_s{0};
    size_t points{0};
    size_t length_s{0};
    std::vector<Line> lines;
};
Chart s_chart;
std::vector<ImVec2> s_points;

void refresh(AppState& app_state, uint64_t encounter_id) {
    const auto now = std::chrono::steady_clock::now();
    const bool same = s_fetched.valid && s_fetched.encounter_id == encounter_id;
    if (same && (encounter_id != 0 || now - s_fetched.at < kLiveRefresh)) return;
    s_fetched.timeline = app_state.get_timeline(encounter_id);
    s_fetched.valid = true;
    s_fetched.encounter_id = encounter_id;
    s_fetched.at = now;
    ++s_fetched.version;
}

/// Seconds the chart spans: the pull's own clock, which a kill stops at the last hit.
size_t chart_length(const meter::EncounterSummary& summary) {
    size_t longest = 0;
    for (const meter::TimelineRow& row : s_fetched.timeline.rows) longest = std::max(longest, row.bins.size());
    const auto duration = static_cast<size_t>(std::ceil(std::max(summary.duration_seconds, 0.0)));
    return std::clamp<size_t>(duration > 0 ? duration : longest, 1, meter::TIMELINE_MAX_SECONDS);
}

void build_chart(const meter::EncounterSummary& summary, size_t points) {
    const meter::DpsMetric dps = selected_dps_metric();
    const size_t length = chart_length(summary);
    if (s_chart.version == s_fetched.version && s_chart.metric == s_metric && s_chart.dps == dps
        && s_chart.smoothing_s == s_smoothing_s && s_chart.points == points && s_chart.length_s == length) {
        return;
    }
    s_chart = Chart{s_fetched.version, s_metric, dps, s_smoothing_s, points, length, {}};
    for (const meter::TimelineRow& row : s_fetched.timeline.rows) {
        const auto who = std::find_if(summary.combatants.begin(), summary.combatants.end(),
            [&row](const meter::CombatantStats& c) { return c.entity_id == row.entity; });
        // A pet the tick has not merged into its owner yet.
        if (who == summary.combatants.end()) continue;
        Line line;
        for (size_t i = 0; i < row.bins.size() && i < length; ++i) {
            line.total += meter::timeline_value(row.bins[i], s_metric, dps);
        }
        if (line.total <= 0.0) continue;
        line.entity = row.entity;
        line.name = who->name;
        line.color = get_job_color_u32(who->job);
        line.values = meter::downsample(meter::smoothed_series(row.bins, length, s_metric, dps, s_smoothing_s), points);
        line.peak = *std::max_element(line.values.begin(), line.values.end());
        s_chart.lines.push_back(std::move(line));
    }
    std::sort(s_chart.lines.begin(), s_chart.lines.end(),
              [](const Line& a, const Line& b) { return a.total > b.total; });
}

void metric_button(const char* label, meter::TimelineMetric metric) {
    if (button(label, s_metric == metric ? ButtonKind::Primary : ButtonKind::Secondary, ButtonSize::Fit)) {
        s_metric = metric;
    }
}

void smoothing_button(const char* label, size_t seconds) {
    if (button(label, s_smoothing_s == seconds ? ButtonKind::Primary : ButtonKind::Secondary, ButtonSize::Fit)) {
        s_smoothing_s = seconds;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Each point averages the %zu seconds around it.", seconds);
    }
}

/// What the lines show on the left, how smooth they are on the right.
void render_controls() {
    const std::string damage =
        std::string(ICON_SWORDS "  ") + std::string(meter::to_string(selected_dps_metric())) + "##TimelineDamage";
    const char* healing = ICON_HEART "  HPS##TimelineHealing";
    const char* taken = ICON_SHIELD "  Damage taken##TimelineTaken";
    metric_button(damage.c_str(), meter::TimelineMetric::Damage);
    same_line_if_room(button_width(healing, ButtonSize::Fit));
    metric_button(healing, meter::TimelineMetric::Healing);
    same_line_if_room(button_width(taken, ButtonSize::Fit));
    metric_button(taken, meter::TimelineMetric::Taken);

    constexpr const char* kLabels[] = {"5 s##TimelineSmooth5", "15 s##TimelineSmooth15", "30 s##TimelineSmooth30"};
    constexpr size_t kSeconds[] = {5, 15, 30};
    const float gap = m(4.0f);
    float width = gap * 2.0f;
    for (const char* label : kLabels) width += button_width(label, ButtonSize::Fit);
    same_line_if_room(width, 24.0f);
    right_align(width);
    for (size_t i = 0; i < std::size(kLabels); ++i) {
        if (i > 0) ImGui::SameLine(0.0f, gap);
        smoothing_button(kLabels[i], kSeconds[i]);
    }
}

/// The top of four gridlines that each read cleanly and leave little room above the peak.
double chart_top(double peak) {
    const double step = std::max(peak, 1.0) / 4.0;
    const double magnitude = std::pow(10.0, std::floor(std::log10(step)));
    for (const double nice : {1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0, 6.0, 8.0}) {
        if (nice * magnitude >= step) return nice * magnitude * 4.0;
    }
    return magnitude * 40.0;
}

/// Seconds between time labels, at least `min_gap` pixels apart.
double time_step(double length_s, float plot_w, float min_gap) {
    for (const double step : {10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0, 900.0, 1800.0}) {
        if (static_cast<double>(plot_w) * step / length_s >= min_gap) return step;
    }
    return 3600.0;
}

std::string clock_label(double seconds) {
    const long s = std::lround(std::max(seconds, 0.0));
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%ld:%02ld", s / 60, s % 60);
    return buf;
}

bool is_shown(const Line& line) {
    return !s_hidden.contains(line.entity);
}

/// What every shown line read at the hovered point, and what was going on then.
void render_readout(const meter::EncounterSummary& summary, const SummaryNames& names, size_t point,
                    double at_s, double span_s) {
    std::vector<const Line*> shown;
    for (const Line& line : s_chart.lines) {
        if (is_shown(line)) shown.push_back(&line);
    }
    std::sort(shown.begin(), shown.end(),
              [point](const Line* a, const Line* b) { return a->values[point] > b->values[point]; });

    ImGui::BeginTooltip();
    ImGui::PushFont(bold_font());
    text_colored_u32(colors::TextPrimary, "%s", clock_label(at_s).c_str());
    ImGui::PopFont();
    for (const Line* line : shown) {
        text_colored_u32(line->color, "%s", line->name.c_str());
        ImGui::SameLine(m(170.0f));
        text_colored_u32(colors::TextPrimary, "%s", format_dps(line->values[point]).c_str());
    }

    bool header = false;
    for (const meter::BuffWindow& window : s_fetched.timeline.buffs) {
        if (at_s < window.begin_s || at_s >= window.end_s) continue;
        if (!header) {
            ImGui::Separator();
            header = true;
        }
        const std::string& giver = names.name(window.source);
        if (window.source == 0 || giver == "Unknown") {
            text_colored_u32(colors::WarningLight, "%s", status_label(window.status).c_str());
        } else {
            text_colored_u32(colors::WarningLight, "%s", status_label(window.status).c_str());
            ImGui::SameLine();
            text_colored_u32(colors::TextDim, "from %s", giver.c_str());
        }
    }
    header = false;
    for (const meter::DeathRecord& death : summary.deaths) {
        if (std::abs(death.time_s - at_s) > span_s * 0.5 + 1.0) continue;
        if (!header) {
            ImGui::Separator();
            header = true;
        }
        text_colored_u32(colors::DangerLight, ICON_SKULL "  %s died at %s", names.name(death.entity).c_str(),
                         clock_label(death.time_s).c_str());
    }
    ImGui::EndTooltip();
}

/// Width of the value labels left of the plot.
float value_gutter(double top) {
    return ImGui::CalcTextSize(format_dps(top).c_str()).x + m(10.0f);
}

void render_chart(const meter::EncounterSummary& summary, float width, float height) {
    const SummaryNames names(summary);
    const float line_h = ImGui::GetTextLineHeight();
    float peak = 0.0f;
    for (const Line& line : s_chart.lines) {
        if (is_shown(line)) peak = std::max(peak, line.peak);
    }
    const double top = chart_top(peak);

    const ImVec2 p_min = ImGui::GetCursorScreenPos();
    const ImVec2 p_max(p_min.x + width, p_min.y + height);
    // Values to the left of the plot, deaths above it, the time along its bottom.
    const ImVec2 plot_min(p_min.x + value_gutter(top), p_min.y + line_h + m(6.0f));
    const ImVec2 plot_max(p_max.x, p_max.y - line_h - m(4.0f));
    const float plot_w = plot_max.x - plot_min.x;
    const float plot_h = plot_max.y - plot_min.y;
    const double length = static_cast<double>(s_chart.length_s);
    const auto x_of = [&](double s) { return plot_min.x + static_cast<float>(std::clamp(s / length, 0.0, 1.0)) * plot_w; };
    const auto y_of = [&](double v) { return plot_max.y - static_cast<float>(std::clamp(v / top, 0.0, 1.0)) * plot_h; };

    ImGui::InvisibleButton("##TimelineCanvas", ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered() && ImGui::GetMousePos().x >= plot_min.x;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilledMultiColor(plot_min, plot_max, colors::SurfaceSunken, colors::SurfaceSunken,
                                colors::SurfaceLow, colors::SurfaceLow);
    dl->AddRect(plot_min, plot_max, colors::Border, m(6.0f), 0, m(1.0f));

    dl->PushClipRect(plot_min, plot_max, true);
    // Raid buffs behind everything, darker where more of them overlap.
    for (const meter::BuffWindow& window : s_fetched.timeline.buffs) {
        const float x0 = x_of(window.begin_s);
        const float x1 = x_of(window.end_s);
        dl->AddRectFilled(ImVec2(x0, plot_min.y), ImVec2(x1, plot_max.y), colors::with_alpha(colors::WarningLight, 0.05f));
        dl->AddLine(ImVec2(x0, plot_min.y + m(1.0f)), ImVec2(x1, plot_min.y + m(1.0f)),
                    colors::with_alpha(colors::WarningLight, 0.55f), m(2.0f));
    }
    for (int i = 1; i <= 3; ++i) {
        const float y = plot_max.y - plot_h * static_cast<float>(i) / 4.0f;
        dl->AddLine(ImVec2(plot_min.x + m(1.0f), y), ImVec2(plot_max.x - m(1.0f), y),
                    colors::with_alpha(colors::Border, 0.45f));
    }
    const double step = time_step(length, plot_w, m(70.0f));
    for (double t = step; t < length; t += step) {
        const float x = x_of(t);
        dl->AddLine(ImVec2(x, plot_min.y + m(1.0f)), ImVec2(x, plot_max.y - m(1.0f)),
                    colors::with_alpha(colors::Border, 0.30f));
    }
    // A whole-pixel width lets ImGui draw the line from its font texture, with half
    // the vertices.
    const float thickness = std::max(1.0f, std::round(m(2.0f)));
    for (const Line& line : s_chart.lines) {
        if (!is_shown(line) || line.values.size() < 2) continue;
        s_points.clear();
        const double span = length / static_cast<double>(line.values.size());
        for (size_t i = 0; i < line.values.size(); ++i) {
            s_points.emplace_back(x_of((static_cast<double>(i) + 0.5) * span), y_of(line.values[i]));
        }
        dl->AddPolyline(s_points.data(), static_cast<int>(s_points.size()), line.color, ImDrawFlags_None, thickness);
    }
    dl->PopClipRect();

    for (int i = 1; i <= 4; ++i) {
        const float y = plot_max.y - plot_h * static_cast<float>(i) / 4.0f;
        const std::string label = format_dps(top * i / 4.0);
        const float w = ImGui::CalcTextSize(label.c_str()).x;
        dl->AddText(ImVec2(plot_min.x - m(6.0f) - w, y - line_h * 0.5f), colors::TextDim, label.c_str());
    }
    for (double t = 0.0; t <= length; t += step) {
        const std::string label = clock_label(t);
        const float w = ImGui::CalcTextSize(label.c_str()).x;
        const float x = std::clamp(x_of(t) - w * 0.5f, plot_min.x, p_max.x - w);
        dl->AddText(ImVec2(x, plot_max.y + m(3.0f)), colors::TextDim, label.c_str());
    }

    // Each death: a skull over the plot, a bar for as long as they stayed down.
    const float skull_w = ImGui::CalcTextSize(ICON_SKULL).x;
    for (const meter::DeathRecord& death : summary.deaths) {
        if (s_hidden.contains(death.entity) || death.time_s > length) continue;
        const float x = x_of(death.time_s);
        const game::Job job = names.job(death.entity);
        const uint32_t color = job != game::Job::None ? get_job_color_u32(job) : colors::DangerLight;
        const double up_at = death.raised_after_s >= 0.0 ? death.time_s + death.raised_after_s : length;
        dl->AddLine(ImVec2(x, plot_min.y), ImVec2(x, plot_max.y), colors::with_alpha(colors::Danger, 0.45f), m(1.0f));
        dl->AddLine(ImVec2(x, plot_min.y - m(2.0f)), ImVec2(std::max(x_of(up_at), x + m(2.0f)), plot_min.y - m(2.0f)),
                    colors::with_alpha(color, 0.8f), m(2.0f));
        dl->AddText(ImVec2(x - skull_w * 0.5f, p_min.y), color, ICON_SKULL);
    }

    if (!hovered || s_chart.lines.empty()) return;
    const size_t n = s_chart.lines.front().values.size();
    const float mouse_x = ImGui::GetMousePos().x;
    const auto point = static_cast<size_t>(
        std::clamp((mouse_x - plot_min.x) / plot_w * static_cast<float>(n), 0.0f, static_cast<float>(n - 1)));
    const double span = length / static_cast<double>(n);
    const double at_s = (static_cast<double>(point) + 0.5) * span;
    const float x = x_of(at_s);
    dl->AddLine(ImVec2(x, plot_min.y), ImVec2(x, plot_max.y), colors::with_alpha(colors::White, 0.35f));
    for (const Line& line : s_chart.lines) {
        if (is_shown(line)) dl->AddCircleFilled(ImVec2(x, y_of(line.values[point])), m(3.5f), line.color);
    }
    render_readout(summary, names, point, at_s, span);
}

float legend_entry_width(const Line& line) {
    return m(16.0f) + m(6.0f) + ImGui::CalcTextSize(line.name.c_str()).x;
}

/// Height the legend's entries take once they wrap to `width`.
float legend_height(float width) {
    const float gap = m(14.0f);
    int rows = 1;
    float x = 0.0f;
    for (const Line& line : s_chart.lines) {
        const float w = legend_entry_width(line);
        if (x > 0.0f && x + gap + w > width) {
            ++rows;
            x = 0.0f;
        }
        x += (x > 0.0f ? gap : 0.0f) + w;
    }
    const float row_h = ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y + m(4.0f);
    return static_cast<float>(rows) * row_h;
}

/// One key per line, which turns the line off and on.
void render_legend() {
    for (size_t i = 0; i < s_chart.lines.size(); ++i) {
        const Line& line = s_chart.lines[i];
        const float w = legend_entry_width(line);
        if (i > 0) same_line_if_room(w, 14.0f);
        const bool shown = is_shown(line);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::PushID(static_cast<int>(line.entity));
        if (ImGui::InvisibleButton("##TimelineLegend", ImVec2(w, ImGui::GetTextLineHeight()))) {
            if (shown) {
                s_hidden.insert(line.entity);
            } else {
                s_hidden.erase(line.entity);
            }
        }
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();
        if (hovered) ImGui::SetTooltip(shown ? "Hide this line" : "Show this line");

        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float mid = p.y + ImGui::GetTextLineHeight() * 0.5f;
        dl->AddLine(ImVec2(p.x, mid), ImVec2(p.x + m(16.0f), mid),
                    shown ? line.color : colors::with_alpha(line.color, 0.25f), m(2.5f));
        dl->AddText(ImVec2(p.x + m(22.0f), p.y),
                    !shown ? colors::TextFaint : (hovered ? colors::TextPrimary : colors::TextBody),
                    line.name.c_str());
    }
}

const char* empty_title() {
    if (s_fetched.timeline.rows.empty()) return "No timeline yet";
    switch (s_metric) {
        case meter::TimelineMetric::Healing: return "No healing this pull";
        case meter::TimelineMetric::Taken: return "No damage taken this pull";
        default: return "No damage this pull";
    }
}

} // namespace

void render_timeline(AppState& app_state, const meter::EncounterSummary& summary, uint64_t encounter_id,
                     float height) {
    const float top = ImGui::GetCursorPosY();
    render_controls();
    ImGui::Dummy(ImVec2(0.0f, m(4.0f)));

    refresh(app_state, encounter_id);
    const float width = ImGui::GetContentRegionAvail().x;
    // One point per pixel across the plot, whose value labels are rarely wider than these.
    build_chart(summary, static_cast<size_t>(std::max(width - value_gutter(999'999.0), 2.0f)));
    if (s_chart.lines.empty()) {
        empty_state(ICON_TRENDING, empty_title(),
                    "Each party member's pull fills in here second by second as it runs.");
        return;
    }

    const float body_h = height - (ImGui::GetCursorPosY() - top);
    const float chart_h = std::max(body_h - legend_height(width) - m(6.0f), m(140.0f));
    render_chart(summary, width, chart_h);
    ImGui::Dummy(ImVec2(0.0f, m(2.0f)));
    render_legend();
}

#endif // HAVE_IMGUI

} // namespace hub::app::ui
