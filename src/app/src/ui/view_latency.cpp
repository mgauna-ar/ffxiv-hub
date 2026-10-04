#include "app/ui/view_latency.hpp"
#include "app/mitigator_impact.hpp"
#include "app/ui/config_binding.hpp"
#include "common/os/local_time.hpp"
#include "common/ui/icons.hpp"
#include "app/ui/overlay_settings.hpp"
#include "app/ui/plugin_page.hpp"
#include "app/ui/theme.hpp"
#include "app/ui/widgets.hpp"
#include "mitigator/types.hpp"
#include "hub/game/actions.hpp"
#include "hub/plugin_registry.hpp"
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <ctime>
#include <optional>
#include <string>
#include <iterator>
#include <string_view>
#include <vector>

namespace hub::app::ui {

#ifdef HAVE_IMGUI

namespace {

constexpr const char* MITI = plugins::LATENCY_MITIGATOR.config_section;

/// What the write-back is doing, as the page header names it.
enum class MitigationMode { Mitigating, DryRun, Off };

MitigationMode mitigation_mode() {
    if (cfg_get(MITI, "dry_run", false)) return MitigationMode::DryRun;
    if (!cfg_get(MITI, "enabled", true)) return MitigationMode::Off;
    return MitigationMode::Mitigating;
}

float lock_ceiling_ms() {
    return cfg_get(MITI, "max_animation_lock_ms",
                   static_cast<float>(mitigator::constants::DEFAULT_MAX_ANIMATION_LOCK_MS));
}

/// Same grading bands as the in-game HUD dot.
uint32_t ping_grade_color(double ping_ms) {
    if (ping_ms < 0.0) return colors::TextDim;
    if (ping_ms < mitigator::constants::PING_GRADE_GOOD_MS)  return colors::SuccessLight;
    if (ping_ms <= mitigator::constants::PING_GRADE_FAIR_MS) return colors::Success;
    if (ping_ms <= mitigator::constants::PING_GRADE_POOR_MS) return colors::WarningLight;
    return colors::Danger;
}

// Colours of the with/without comparison, shared by the card, the chart and the table.
constexpr uint32_t kRttColor      = colors::with_alpha(colors::TextFaint, 0.55f);
constexpr uint32_t kWithoutColor  = colors::with_alpha(colors::TextMuted, 0.70f);
constexpr uint32_t kWithLockColor = colors::Accent;
constexpr uint32_t kWithLineColor = colors::AccentHover;
constexpr uint32_t kSavedColor    = colors::Success;

uint32_t with_color(MitigationMode mode) {
    return mode == MitigationMode::DryRun ? colors::Violet : kWithLockColor;
}

/// Width legend_entry() takes, so the header slot fits the legend exactly.
float legend_entry_width(const char* label) {
    return m(18.0f) + m(5.0f) + ImGui::CalcTextSize(label).x;
}

/// One legend key in a section header's action slot. That row is a button tall
/// and SameLine returns to its top, so each entry centres itself on it.
void legend_entry(const char* label, uint32_t color) {
    const float y = ImGui::GetCursorPosY() +
                    (m(metrics::ButtonH) - ImGui::GetTextLineHeight()) * 0.5f;
    ImGui::SetCursorPosY(y);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float mid = p.y + ImGui::GetTextLineHeight() * 0.5f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, mid), ImVec2(p.x + m(14.0f), mid), color, m(2.2f));
    ImGui::Dummy(ImVec2(m(18.0f), ImGui::GetTextLineHeight()));
    ImGui::SameLine(0.0f, m(5.0f));
    ImGui::SetCursorPosY(y);
    text_colored_u32(colors::TextDim, "%s", label);
}

// ---------------------------------------------------------------------------
// Without vs with card
// ---------------------------------------------------------------------------

/// A filled bar segment, with its label centred inside when it fits.
void bar_segment(ImDrawList* dl, float x0, float x1, float y0, float y1, uint32_t fill,
                 const char* label) {
    if (x1 <= x0) return;
    dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), fill, m(3.0f));
    const ImVec2 size = ImGui::CalcTextSize(label);
    if (size.x + m(8.0f) <= x1 - x0) {
        dl->AddText(ImVec2((x0 + x1 - size.x) * 0.5f, (y0 + y1 - size.y) * 0.5f),
                    colors::TextPrimary, label);
    }
}

void render_impact_card(const ImpactSummary& s, MitigationMode mode) {
    CardOptions opts{};
    opts.auto_height = true;
    begin_card("##ImpactCard", ImVec2(0.0f, 0.0f), opts);
    section_header(ICON_BOLT, "TIME PER ABILITY: WITHOUT VS WITH MITIGATION", kSavedColor);

    if (s.samples == 0) {
        text_dim("Use an ability in game and the comparison fills in.");
        end_card();
        return;
    }

    const float label_w = ImGui::CalcTextSize("Without").x + m(14.0f);
    const float total_w = ImGui::CalcTextSize("0000 ms").x + m(12.0f);
    const float bar_h = ImGui::GetTextLineHeight() + m(10.0f);
    const float row_gap = m(6.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float bar_x0 = origin.x + label_w;
    const float bar_w = std::max(ImGui::GetContentRegionAvail().x - label_w - total_w, m(120.0f));
    const float scale = bar_w / std::max(s.avg_without_ms, 1.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();

    char rtt_label[48], lock_label[48], total[24];
    std::snprintf(rtt_label, sizeof(rtt_label), "round trip %.0f ms", s.avg_rtt_ms);

    // Without: the round trip, then the whole lock the server sent.
    float y = origin.y;
    const float text_dy = (bar_h - ImGui::GetTextLineHeight()) * 0.5f;
    dl->AddText(ImVec2(origin.x, y + text_dy), colors::TextMuted, "Without");
    const float rtt_x1 = bar_x0 + s.avg_rtt_ms * scale;
    bar_segment(dl, bar_x0, rtt_x1, y, y + bar_h, kRttColor, rtt_label);
    std::snprintf(lock_label, sizeof(lock_label), "lock %.0f ms", s.avg_server_lock_ms);
    const float without_x1 = bar_x0 + s.avg_without_ms * scale;
    bar_segment(dl, rtt_x1, without_x1, y, y + bar_h, kWithoutColor, lock_label);
    std::snprintf(total, sizeof(total), "%.0f ms", s.avg_without_ms);
    dl->AddText(ImVec2(without_x1 + m(8.0f), y + text_dy), colors::TextBody, total);

    // With: the same round trip, then the lock actually written, then what was cut.
    y += bar_h + row_gap;
    dl->AddText(ImVec2(origin.x, y + text_dy), colors::TextBody, "With");
    bar_segment(dl, bar_x0, rtt_x1, y, y + bar_h, kRttColor, rtt_label);
    std::snprintf(lock_label, sizeof(lock_label), "lock %.0f ms", s.avg_applied_lock_ms);
    const float with_x1 = bar_x0 + s.avg_with_ms * scale;
    bar_segment(dl, rtt_x1, with_x1, y, y + bar_h, with_color(mode), lock_label);
    if (without_x1 - with_x1 > m(2.0f)) {
        dl->AddRectFilled(ImVec2(with_x1, y), ImVec2(without_x1, y + bar_h),
                          colors::with_alpha(kSavedColor, 0.12f), m(3.0f));
        dl->AddRect(ImVec2(with_x1, y), ImVec2(without_x1, y + bar_h),
                    colors::with_alpha(kSavedColor, 0.65f), m(3.0f), 0, m(1.2f));
        char saved[32];
        std::snprintf(saved, sizeof(saved), "-%.0f ms", s.avg_saved_ms());
        const ImVec2 size = ImGui::CalcTextSize(saved);
        if (size.x + m(8.0f) <= without_x1 - with_x1) {
            dl->AddText(ImVec2((with_x1 + without_x1 - size.x) * 0.5f, y + text_dy),
                        colors::SuccessLight, saved);
        }
    }
    std::snprintf(total, sizeof(total), "%.0f ms", s.avg_with_ms);
    dl->AddText(ImVec2(without_x1 + m(8.0f), y + text_dy), colors::TextPrimary, total);

    ImGui::Dummy(ImVec2(0.0f, bar_h * 2.0f + row_gap + m(4.0f)));

    // The headline: one sentence that answers "what does it change for me".
    const float saved_ms = s.avg_saved_ms();
    const float percent = s.avg_without_ms > 0.0f ? 100.0f * saved_ms / s.avg_without_ms : 0.0f;
    ImGui::PushFont(bold_font());
    if (mode == MitigationMode::Off) {
        text_colored_u32(colors::DangerLight,
                         "Mitigation is off: new abilities wait the full time, like the Without bar.");
    } else if (saved_ms < 1.0f) {
        text_colored_u32(colors::TextMuted, "Nothing trimmed recently: your round trip is already near the target.");
    } else if (mode == MitigationMode::DryRun) {
        text_colored_u32(colors::Violet, "Would be %.0f ms sooner after every ability (-%.0f%%). Dry-run writes nothing.",
                         saved_ms, percent);
    } else {
        text_colored_u32(colors::SuccessLight, "%.0f ms sooner after every ability (-%.0f%%)", saved_ms, percent);
    }
    ImGui::PopFont();
    ImGui::SameLine(0.0f, m(12.0f));
    text_dim("average of your last %zu abilities; casts are left out, their lock is never trimmed", s.samples);

    end_card();
}

// ---------------------------------------------------------------------------
// Time per ability chart
// ---------------------------------------------------------------------------

struct ChartPoint {
    WeaveTiming timing;
    uint32_t action_id;
    bool spike_filtered;
};

void render_weave_chart(const std::vector<ChartPoint>& points, MitigationMode mode, float height) {
    const ImVec2 canvas_size(ImGui::GetContentRegionAvail().x, height);
    const ImVec2 p_min = ImGui::GetCursorScreenPos();
    const ImVec2 p_max(p_min.x + canvas_size.x, p_min.y + canvas_size.y);
    const float rounding = m(6.0f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilledMultiColor(p_min, p_max, colors::SurfaceSunken, colors::SurfaceSunken,
                                colors::SurfaceLow, colors::SurfaceLow);
    dl->AddRect(p_min, p_max, colors::Border, rounding, 0, m(1.0f));

    if (points.size() < 2) {
        ImGui::Dummy(canvas_size);
        ImGui::SetCursorScreenPos(ImVec2(p_min.x, p_min.y + canvas_size.y * 0.18f));
        ImGui::BeginGroup();
        empty_state(ICON_ACTIVITY, "Awaiting action telemetry",
                    "Use a few abilities in game and the comparison fills in.");
        ImGui::EndGroup();
        ImGui::SetCursorScreenPos(ImVec2(p_min.x, p_max.y));
        return;
    }

    // The axis starts at zero so the gap between the lines is to scale.
    float max_ms = 800.0f;
    // Filtered spikes are left out of the scale: they run off the top rather than
    // squash the two lines every ordinary action sits on.
    for (const auto& p : points) {
        if (!p.spike_filtered) max_ms = std::max(max_ms, p.timing.without_ms);
    }
    // The finest step whose labels still have room between them at this height.
    const float label_gap = ImGui::GetTextLineHeight() * 1.8f;
    const float plot_h = canvas_size.y - m(20.0f);
    float step = 1000.0f;
    for (const float candidate : {100.0f, 200.0f, 250.0f, 500.0f}) {
        if (std::ceil(max_ms / candidate) * label_gap <= plot_h) {
            step = candidate;
            break;
        }
    }
    max_ms = std::ceil(max_ms / step) * step;

    // Axis labels sit in their own column on the right, so the lines never cover them.
    const float axis_w = ImGui::CalcTextSize("0000 ms").x + m(16.0f);
    const float plot_x0 = p_min.x + m(10.0f);
    const float plot_x1 = p_max.x - axis_w;
    const float plot_y0 = p_min.y + m(10.0f);
    const float plot_y1 = p_max.y - m(10.0f);
    const auto y_for = [&](float value) {
        return plot_y1 - (std::clamp(value, 0.0f, max_ms) / max_ms) * (plot_y1 - plot_y0);
    };

    for (float v = 0.0f; v <= max_ms + 0.5f; v += step) {
        const float y = y_for(v);
        dl->AddLine(ImVec2(plot_x0, y), ImVec2(plot_x1, y), colors::with_alpha(colors::Border, 0.45f));
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%.0f ms", v);
        const ImVec2 size = ImGui::CalcTextSize(buf);
        dl->AddText(ImVec2(plot_x1 + m(8.0f), y - size.y * 0.5f),
                    colors::with_alpha(colors::TextDim, 0.85f), buf);
    }

    const float dx = (plot_x1 - plot_x0) / static_cast<float>(points.size() - 1);
    const auto x_for = [&](size_t i) { return plot_x0 + static_cast<float>(i) * dx; };

    dl->PushClipRect(p_min, p_max, true);
    // Saved time: the band between what it would have been and what it was.
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        const float x0 = x_for(i), x1 = x_for(i + 1);
        dl->AddQuadFilled(ImVec2(x0, y_for(points[i].timing.without_ms)),
                          ImVec2(x1, y_for(points[i + 1].timing.without_ms)),
                          ImVec2(x1, y_for(points[i + 1].timing.with_ms)),
                          ImVec2(x0, y_for(points[i].timing.with_ms)),
                          colors::with_alpha(kSavedColor, 0.18f));
    }
    const uint32_t with_line = mode == MitigationMode::DryRun ? colors::Violet : kWithLineColor;
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        dl->AddLine(ImVec2(x_for(i), y_for(points[i].timing.without_ms)),
                    ImVec2(x_for(i + 1), y_for(points[i + 1].timing.without_ms)), kWithoutColor, m(1.5f));
    }
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        dl->AddLine(ImVec2(x_for(i), y_for(points[i].timing.with_ms)),
                    ImVec2(x_for(i + 1), y_for(points[i + 1].timing.with_ms)), with_line, m(2.2f));
    }
    dl->PopClipRect();

    // Hover crosshair and readout.
    const ImVec2 mouse = ImGui::GetMousePos();
    if (mouse.x >= plot_x0 - dx * 0.5f && mouse.x <= plot_x1 + dx * 0.5f &&
        mouse.y >= p_min.y && mouse.y <= p_max.y) {
        const int idx = static_cast<int>((mouse.x - plot_x0) / dx + 0.5f);
        if (idx >= 0 && idx < static_cast<int>(points.size())) {
            const auto& p = points[static_cast<size_t>(idx)];
            const float hover_x = x_for(static_cast<size_t>(idx));
            dl->AddLine(ImVec2(hover_x, plot_y0), ImVec2(hover_x, plot_y1),
                        colors::with_alpha(colors::White, 0.35f));
            dl->AddCircleFilled(ImVec2(hover_x, y_for(p.timing.without_ms)), m(3.0f), kWithoutColor);
            dl->AddCircleFilled(ImVec2(hover_x, y_for(p.timing.with_ms)), m(3.5f), colors::White);

            ImGui::BeginTooltip();
            const std::string_view name = hub::game::action_sheet_name(p.action_id);
            if (name.empty()) {
                text_colored_u32(colors::TextPrimary, "Action #%u", p.action_id);
            } else {
                text_colored_u32(colors::TextPrimary, "%.*s", static_cast<int>(name.size()), name.data());
            }
            text_colored_u32(colors::TextMuted, "Round trip: %.0f ms", p.timing.rtt_ms);
            text_colored_u32(colors::TextBody, "Without: %.0f ms", p.timing.without_ms);
            text_colored_u32(with_line, "With: %.0f ms", p.timing.with_ms);
            text_colored_u32(colors::SuccessLight, "Saved: %.0f ms", p.timing.saved_ms());
            if (p.spike_filtered) {
                text_colored_u32(colors::Warning, ICON_WARNING "  Spike: trimmed by the usual round trip");
            }
            ImGui::EndTooltip();
        }
    }

    ImGui::Dummy(canvas_size);
}

// ---------------------------------------------------------------------------
// Tiles
// ---------------------------------------------------------------------------

void render_metric_tiles(AppState& app_state, const AppState::MitigatorMetrics& metrics,
                         const ImpactSummary& impact) {
    const float target_ping = cfg_get(MITI, "target_ping_ms", 15.0f);
    const float floor_ms = cfg_get(MITI, "min_animation_lock_ms", 25.0f);

    char delay[32];
    std::snprintf(delay, sizeof(delay), "%.1f ms", metrics.latest_smoothed_rtt_ms);
    char delay_sub[48];
    std::snprintf(delay_sub, sizeof(delay_sub), "press to server reply (last %.0f ms)",
                  metrics.latest_measured_rtt_ms);
    char delay_tip[256];
    std::snprintf(delay_tip, sizeof(delay_tip),
                  "How long the server takes to answer an ability, averaged. Without mitigation you "
                  "wait this long on top of every lock; mitigation takes everything above %.0f ms "
                  "off the lock.", target_ping);

    // Hub-measured ICMP ping to the game server. A separate metric from the
    // action delay above, so it gets its own tile rather than sharing one.
    const double net_ping = app_state.network_ping_ms();
    char net[32];
    if (net_ping >= 0.0) {
        std::snprintf(net, sizeof(net), "%.0f ms", net_ping);
    } else {
        std::snprintf(net, sizeof(net), "--");
    }

    char jitter[32];
    std::snprintf(jitter, sizeof(jitter), "+/- %.1f ms", metrics.latest_jitter_ms);
    char jitter_tip[256];
    std::snprintf(jitter_tip, sizeof(jitter_tip),
                  "How much the action delay moves from one ability to the next. Lower is steadier. "
                  "%llu spikes were ignored so far, and %llu trims stopped at the %.0f ms safety floor.",
                  static_cast<unsigned long long>(metrics.spike_filtered_count),
                  static_cast<unsigned long long>(metrics.floor_clamp_count), floor_ms);

    char saved[32];
    if (impact.samples > 0) {
        std::snprintf(saved, sizeof(saved), "%.0f ms", impact.avg_saved_ms());
    } else {
        std::snprintf(saved, sizeof(saved), "--");
    }
    char saved_sub[64];
    std::snprintf(saved_sub, sizeof(saved_sub), "per ability; %.1f s in total",
                  metrics.total_delay_reduced_ms / 1000.0f);

    const StatTileSpec tiles[] = {
        { "##PingCard", ICON_ACTIVITY, "ACTION DELAY", delay,
          ping_grade_color(metrics.latest_smoothed_rtt_ms), delay_sub, colors::Accent },
        { "##NetPingCard", ICON_ACTIVITY, "NETWORK PING", net,
          ping_grade_color(net_ping), net_ping >= 0.0 ? "plain ping to the server" : "not measured yet",
          colors::Accent },
        { "##JitterCard", ICON_TRENDING, "STABILITY", jitter,
          colors::SuccessLight, "how much the delay varies", colors::Success },
        { "##ReducedCard", ICON_BOLT, "TIME SAVED", saved,
          colors::WarningLight, saved_sub, colors::Warning },
    };
    const char* const tooltips[] = {
        delay_tip,
        "Plain network ping to the game server, or to your data center's lobby when the server "
        "does not answer. Always lower than Action delay, which also includes the server "
        "processing the ability.",
        jitter_tip,
        "How much sooner you can act after each ability, averaged over your recent abilities. "
        "Below it, the total removed this session. Dry-run and switched-off abilities add nothing "
        "to the total.",
    };
    stat_tile_row(tiles, std::size(tiles), tooltips);
}

// ---------------------------------------------------------------------------
// Action table
// ---------------------------------------------------------------------------

/// What happened to the lock, then what shaped the trim. A spike or the floor
/// only changes how much was taken off, so it never replaces "Mitigated".
void render_status_cell(const ipc::MitigatorTelemetryPayload& s, float floor_ms) {
    ImGui::BeginGroup();
    if (s.dry_run) {
        text_colored_u32(colors::Violet, ICON_FLASK "  Dry-run (not applied)");
    } else if (s.cast_active) {
        text_colored_u32(colors::Violet, ICON_HOURGLASS "  Cast - skipped");
    } else if (s.applied) {
        text_colored_u32(colors::SuccessLight, ICON_CHECK "  Mitigated");
    } else {
        text_colored_u32(colors::TextFaint, "No change");
    }
    if (s.spike_filtered) {
        ImGui::SameLine(0.0f, m(10.0f));
        text_colored_u32(colors::Warning, ICON_WARNING " spike filtered");
    }
    if (s.cold_start_guard) {
        ImGui::SameLine(0.0f, m(10.0f));
        text_colored_u32(colors::Warning, ICON_TIMER " cold start");
    }
    if (s.clamped_floor) {
        ImGui::SameLine(0.0f, m(10.0f));
        text_colored_u32(colors::Violet, ICON_SHIELD " at floor");
    }
    ImGui::EndGroup();

    if (!(s.spike_filtered || s.cold_start_guard || s.clamped_floor) || !ImGui::IsItemHovered()) {
        return;
    }
    ImGui::BeginTooltip();
    if (s.spike_filtered) {
        text_colored_u32(colors::Warning, ICON_WARNING "  Spike filtered");
        text_colored_u32(colors::TextMuted, "This round trip was a spike, so the median set the trim.");
    }
    if (s.cold_start_guard) {
        text_colored_u32(colors::Warning, ICON_TIMER "  Cold start");
        text_colored_u32(colors::TextMuted, "Fewer than five samples so far, so the round trip was capped.");
    }
    if (s.clamped_floor) {
        text_colored_u32(colors::Violet, ICON_SHIELD "  At floor");
        text_colored_u32(colors::TextMuted, "Stopped at the safety floor (%.0f ms).", floor_ms);
    }
    ImGui::EndTooltip();
}

/// One column of the action table: its header and what hovering that header explains.
struct FeedColumn {
    const char* name;
    const char* tooltip;
};

constexpr FeedColumn kFeedColumns[] = {
    { "Time", nullptr },
    { "Action", "Hover a name for its action id and sequence number." },
    { "RTT", "Round trip: from sending the ability to the server's reply, in ms." },
    { "Server lock", "Animation lock the server sent, in ms." },
    { "Applied lock", "Animation lock written into the game, in ms." },
    { "Without", "Time until you could act again without mitigation: round trip + server lock." },
    { "With", "Time until you could act again with mitigation: round trip + applied lock." },
    { "Saved", "Without minus With." },
    { "Status", nullptr },
};

void render_action_feed(const std::vector<ipc::MitigatorTelemetryPayload>& telemetry, float floor_ms,
                        float ceiling_ms) {
    if (telemetry.empty()) {
        empty_state(ICON_CHECKLIST, "No actions recorded yet",
                    "Every ability the client sends shows up here as it happens.");
        return;
    }

    constexpr int kColumns = static_cast<int>(std::size(kFeedColumns));
    const auto sizing = table_sizing(820.0f, kColumns, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                                       ImGuiTableFlags_BordersInnerV);
    if (!ImGui::BeginTable("##RecentActionsTable", kColumns, sizing.flags, ImVec2(0.0f, fill_h(0.0f)))) {
        return;
    }

    ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, m(66.0f));
    ImGui::TableSetupColumn("Action", sizing.flex_flags(), sizing.flex_width(150.0f, 1.5f));
    ImGui::TableSetupColumn("RTT", ImGuiTableColumnFlags_WidthFixed, m(52.0f));
    ImGui::TableSetupColumn("Server lock", ImGuiTableColumnFlags_WidthFixed, m(78.0f));
    ImGui::TableSetupColumn("Applied lock", ImGuiTableColumnFlags_WidthFixed, m(84.0f));
    ImGui::TableSetupColumn("Without", ImGuiTableColumnFlags_WidthFixed, m(62.0f));
    ImGui::TableSetupColumn("With", ImGuiTableColumnFlags_WidthFixed, m(52.0f));
    ImGui::TableSetupColumn("Saved", ImGuiTableColumnFlags_WidthFixed, m(56.0f));
    ImGui::TableSetupColumn("Status", sizing.flex_flags(), sizing.flex_width(220.0f, 2.2f));
    ImGui::TableSetupScrollFreeze(0, 1);

    // Headers by hand, so each can explain itself on hover.
    ImGui::PushStyleColor(ImGuiCol_Text, v4(colors::TextDim));
    ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
    for (int c = 0; c < kColumns; ++c) {
        ImGui::TableSetColumnIndex(c);
        ImGui::TableHeader(kFeedColumns[c].name);
        if (kFeedColumns[c].tooltip != nullptr && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", kFeedColumns[c].tooltip);
        }
    }
    ImGui::PopStyleColor();

    for (auto it = telemetry.rbegin(); it != telemetry.rend(); ++it) {
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        const std::time_t secs = static_cast<std::time_t>(it->timestamp_ms / 1000);
        const std::tm tm_buf = os::local_time(secs);
        text_colored_u32(colors::TextDim, "%02d:%02d:%02d", tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);

        ImGui::TableSetColumnIndex(1);
        // string_view accessor rather than action_name(): this runs for every
        // visible row every frame and the latter builds a std::string per call.
        const std::string_view action = hub::game::action_sheet_name(it->action_id);
        if (action.empty()) {
            text_colored_u32(colors::TextMuted, "#%u", it->action_id);
        } else {
            text_colored_u32(colors::TextBody, "%.*s", static_cast<int>(action.size()),
                             action.data());
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Action #%u, sequence %u", it->action_id, it->sequence);
        }

        ImGui::TableSetColumnIndex(2);
        if (it->measured_rtt_ms > 0.0f) {
            text_colored_u32(colors::TextBody, "%.0f", it->measured_rtt_ms);
        } else {
            text_colored_u32(colors::TextFaint, "--");
        }

        // Server and applied side by side: without both there is no way to see
        // whether an action was actually mitigated.
        ImGui::TableSetColumnIndex(3);
        text_colored_u32(colors::TextMuted, "%.0f", it->original_lock_ms);

        ImGui::TableSetColumnIndex(4);
        text_colored_u32(colors::TextBody, "%.0f", it->adjusted_lock_ms);

        const std::optional<WeaveTiming> timing = weave_timing(*it, ceiling_ms);
        ImGui::TableSetColumnIndex(5);
        if (timing) {
            text_colored_u32(colors::TextMuted, "%.0f", timing->without_ms);
        } else {
            text_colored_u32(colors::TextFaint, "--");
        }
        ImGui::TableSetColumnIndex(6);
        if (timing) {
            text_colored_u32(colors::TextPrimary, "%.0f", timing->with_ms);
        } else {
            text_colored_u32(colors::TextFaint, "--");
        }

        ImGui::TableSetColumnIndex(7);
        if (it->delay_reduced_ms > 0.0f) {
            text_colored_u32(colors::SuccessLight, "-%.0f", it->delay_reduced_ms);
        } else {
            text_colored_u32(colors::TextFaint, "--");
        }

        ImGui::TableSetColumnIndex(8);
        render_status_cell(*it, floor_ms);
    }

    ImGui::EndTable();
}

void render_live_tab(AppState& app_state, const AppState::MitigatorMetrics& metrics,
                     const std::vector<ipc::MitigatorTelemetryPayload>& telemetry) {
    const MitigationMode mode = mitigation_mode();
    const float ceiling_ms = lock_ceiling_ms();
    const ImpactSummary impact = summarize_impact(telemetry, ceiling_ms);

    render_metric_tiles(app_state, metrics, impact);
    ImGui::Dummy(ImVec2(0.0f, m(4.0f)));
    render_impact_card(impact, mode);
    ImGui::Dummy(ImVec2(0.0f, m(2.0f)));

    // Chart takes a fixed share of what is left; the feed takes the rest, so both
    // reach the bottom of the window instead of stopping at a magic pixel height.
    const float remaining = fill_h(0.0f);
    const float graph_card_h = std::max(remaining * 0.38f, m(170.0f));

    begin_card("##RttGraphCard", ImVec2(0.0f, graph_card_h));
    const float legend_w = legend_entry_width("without") + legend_entry_width("with") +
                           legend_entry_width("saved") + m(12.0f) * 2.0f;
    begin_section_header(ICON_TRENDING, "TIME PER ABILITY, RECENT ACTIONS", legend_w);
    legend_entry("without", kWithoutColor);
    ImGui::SameLine(0.0f, m(12.0f));
    legend_entry("with", mode == MitigationMode::DryRun ? colors::Violet : kWithLineColor);
    ImGui::SameLine(0.0f, m(12.0f));
    legend_entry("saved", colors::with_alpha(kSavedColor, 0.75f));
    end_section_header();

    std::vector<ChartPoint> points;
    points.reserve(telemetry.size());
    for (const auto& s : telemetry) {
        if (const auto t = weave_timing(s, ceiling_ms)) {
            points.push_back({ *t, s.action_id, s.spike_filtered != 0 });
        }
    }
    render_weave_chart(points, mode, fill_h(0.0f));
    end_card();

    ImGui::Dummy(ImVec2(0.0f, m(2.0f)));

    begin_card("##ActionFeedCard", ImVec2(0.0f, fill_h(0.0f)));
    section_header(ICON_CHECKLIST, "RECENT ACTION TELEMETRY", colors::Violet);
    render_action_feed(telemetry, cfg_get(MITI, "min_animation_lock_ms", 25.0f), ceiling_ms);
    end_card();
}

// ---------------------------------------------------------------------------
// Settings sections. Order and headings are the same for every plugin: what the
// plugin does, then the in-game overlay, then how it is displayed, then the
// destructive actions.
// ---------------------------------------------------------------------------

void render_plugin_section(AppState& app_state) {
    begin_settings_card("##MitiPluginCard", ICON_GAUGE, "MITIGATION ALGORITHM", colors::Accent);

    // Distinct from the plugin's master switch in the page header: this one only
    // stops the memory write-back, leaving measurement running.
    bool enabled = cfg_get(MITI, "enabled", true);
    if (setting_toggle("Enable animation lock mitigation",
                       "Master switch for every memory write this plugin makes.", &enabled)) {
        cfg_store(MITI, "enabled", enabled);
        app_state.send_command(PluginId::LatencyMitigator, CommandId::SetMitigationEnabled, enabled ? 1 : 0);
    }

    // The sliders below reach the payload and the file on release: nothing
    // in-game previews them, and a drag would otherwise save on every frame.
    float target_ping = cfg_get(MITI, "target_ping_ms", 15.0f);
    begin_setting_row("Target ping", "Round-trip time the compensation aims for.");
    if (ImGui::SliderFloat("##target_ping", &target_ping, 10.0f, 40.0f, "%.1f ms")) {
        cfg_set(MITI, "target_ping_ms", target_ping);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        cfg_save();
        app_state.send_command(PluginId::LatencyMitigator, CommandId::SetTargetPing, 0, target_ping);
    }
    end_setting_row();

    float min_lock = cfg_get(MITI, "min_animation_lock_ms", 25.0f);
    begin_setting_row("Safety floor", "Animation lock is never reduced below this.");
    if (ImGui::SliderFloat("##min_lock", &min_lock, 25.0f, 100.0f, "%.1f ms")) {
        cfg_set(MITI, "min_animation_lock_ms", min_lock);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        cfg_save();
        app_state.send_command(PluginId::LatencyMitigator, CommandId::SetMinLock, 0, min_lock);
    }
    end_setting_row();

    float spike_mult = cfg_get(MITI, "spike_multiplier", 2.5f);
    begin_setting_row("Spike multiplier", "How far above the average an RTT is discarded.");
    if (ImGui::SliderFloat("##spike_mult", &spike_mult, 2.0f, 4.0f, "%.1fx")) {
        cfg_set(MITI, "spike_multiplier", spike_mult);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        cfg_save();
        app_state.send_command(PluginId::LatencyMitigator, CommandId::SetSpikeMultiplier, 0, spike_mult);
    }
    end_setting_row();

    bool dry_run = cfg_get(MITI, "dry_run", false);
    if (setting_toggle("Dry-run mode",
                       "Measure and report only; performs zero memory edits.", &dry_run)) {
        cfg_store(MITI, "dry_run", dry_run);
        app_state.send_command(PluginId::LatencyMitigator, CommandId::ToggleDryRun, dry_run ? 1 : 0);
    }

    end_settings_card();
}

void render_overlay_section(AppState& app_state) {
    CardOptions opts{};
    opts.auto_height = true;
    begin_card("##MitiOverlayCard", ImVec2(0.0f, 0.0f), opts);

    OverlaySettingsOptions overlay_opts{};
    overlay_opts.section = MITI;
    overlay_opts.plugin = PluginId::LatencyMitigator;
    overlay_opts.visible_label = "Show micro ping HUD";
    overlay_opts.locked_label = "Lock HUD position";
    overlay_opts.opacity_label = "HUD opacity";
    overlay_opts.scale_label = "HUD scale";
    overlay_opts.min_opacity = mitigator::constants::MIN_OVERLAY_OPACITY;
    overlay_opts.max_opacity = mitigator::constants::MAX_OVERLAY_OPACITY;
    overlay_opts.min_scale = mitigator::constants::MIN_OVERLAY_SCALE;
    overlay_opts.max_scale = mitigator::constants::MAX_OVERLAY_SCALE;
    overlay_opts.defaults = mitigator::default_overlay_config();
    render_overlay_settings(app_state, overlay_opts);

    end_card();
}

void render_display_section(AppState& app_state) {
    begin_settings_card("##MitiDisplayCard", ICON_CHECKLIST, "HUD DISPLAY", colors::Violet);

    int hud_mode = cfg_get(MITI, "overlay_mode", 0);
    static const char* hud_mode_names[] = { "Compact inline", "Two row", "Ping only" };
    begin_setting_row("HUD layout", "How much the in-game HUD shows at a glance.");
    if (ImGui::Combo("##hud_layout", &hud_mode, hud_mode_names, 3)) {
        cfg_store(MITI, "overlay_mode", hud_mode);
        app_state.send_command(PluginId::LatencyMitigator, CommandId::SetOverlayMode, static_cast<uint32_t>(hud_mode));
    }
    end_setting_row();

    end_settings_card();
}

void render_maintenance_section(AppState& app_state) {
    begin_settings_card("##MitiMaintenanceCard", ICON_WRENCH, "MAINTENANCE", colors::Warning);

    if (button(ICON_MOVE "  Reset HUD position", ButtonKind::Secondary, ButtonSize::Large)) {
        app_state.send_command(PluginId::LatencyMitigator, CommandId::ResetOverlayGeometry);
    }
    ImGui::Dummy(ImVec2(0.0f, m(6.0f)));
    if (button(ICON_RESET "  Reset statistics", ButtonKind::Danger, ButtonSize::Large)) {
        app_state.send_command(PluginId::LatencyMitigator, CommandId::ResetStats);
        app_state.clear_mitigator_stats();
    }

    end_settings_card();
}

void render_settings_tab(AppState& app_state) {
    const SettingsSection sections[] = {
        { [&] { render_plugin_section(app_state); } },
        { [&] { render_overlay_section(app_state); } },
        { [&] { render_display_section(app_state); } },
        { [&] { render_maintenance_section(app_state); } },
    };
    render_settings_grid(sections, std::size(sections));
}

} // namespace
#endif

void render_view_latency(AppState& app_state) {
#ifdef HAVE_IMGUI
    // Dry-run is otherwise invisible on this view, so the numbers below look like
    // mitigation that never happened.
    PluginStatus status{ "Mitigating", colors::SuccessLight };
    if (cfg_get(MITI, "dry_run", false)) {
        status = { "Dry-run - measuring only", colors::Violet };
    } else if (!cfg_get(MITI, "enabled", true)) {
        status = { "Mitigation disabled", colors::Danger };
    }

    render_plugin_header(app_state, PluginId::LatencyMitigator, ICON_ACTIVITY,
                         "Latency Mitigator",
                         "Animation lock compensation and slide-cast preservation", status);
    if (render_plugin_disabled_gate(app_state, PluginId::LatencyMitigator,
                                    "Latency Mitigator")) {
        return;
    }

    const auto metrics_snapshot = app_state.get_mitigator_metrics();
    const auto telemetry = app_state.get_recent_telemetry(120);

    const PluginTab tabs[] = {
        {ICON_TRENDING, "Live telemetry",
         [&] { render_live_tab(app_state, metrics_snapshot, telemetry); }},
        {ICON_SLIDERS, "Settings", [&app_state] { render_settings_tab(app_state); }},
    };
    render_plugin_tabs("##LatencyTabs", tabs, std::size(tabs));
#else
    (void)app_state;
#endif
}

} // namespace hub::app::ui
