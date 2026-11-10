#include "meter/combat_overlay.hpp"
#include "common/ui/job_style.hpp"
#include "common/ui/overlay_chrome.hpp"
#include "common/ui/overlay_palette.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace hub::meter {

CombatOverlay::CombatOverlay(EncounterEngine* engine)
    : m_engine(engine) {
    set_geometry(default_geometry());
}

CombatOverlay::~CombatOverlay() = default;

Rect CombatOverlay::default_geometry() const noexcept {
    return Rect{-1.0f, -1.0f,
                static_cast<float>(constants::DEFAULT_WINDOW_WIDTH),
                static_cast<float>(constants::DEFAULT_WINDOW_HEIGHT)};
}

std::vector<const CombatantStats*> CombatOverlay::sorted_combatants(
    const EncounterSummary& summary, bool party_only, bool by_healing, bool hide_inactive, DpsMetric dps_metric
) {
    std::vector<const CombatantStats*> list;
    list.reserve(summary.combatants.size());

    // "Party" is the synced party list; is_friendly() would also admit any other
    // player nearby. Solo play has no list, so it falls back to friendly rows
    // rather than an empty table.
    const bool grouped = std::any_of(summary.combatants.begin(), summary.combatants.end(),
        [](const CombatantStats& c) { return c.is_party_member; });
    const auto in_party = [grouped](const CombatantStats& c) {
        if (!grouped) return c.is_friendly();
        return c.is_party_member || c.is_local_player || c.actor_type == ActorType::LimitBreak;
    };

    for (const auto& c : summary.combatants) {
        if (c.is_pet) continue;  // Merged into the owner's totals.
        if (party_only && !in_party(c)) continue;
        if (c.total_damage == 0 && c.total_healing == 0 && c.damage_taken == 0 && c.buff_given == 0) continue;
        if (hide_inactive) {
            const uint64_t contribution = by_healing ? c.effective_healing : c.total_damage + c.buff_given;
            if (contribution == 0) continue;
        }
        list.push_back(&c);
    }

    if (by_healing) {
        std::sort(list.begin(), list.end(), [](const CombatantStats* a, const CombatantStats* b) {
            if (a->hps != b->hps) return a->hps > b->hps;
            return a->effective_healing > b->effective_healing;
        });
    } else {
        std::sort(list.begin(), list.end(), [dps_metric](const CombatantStats* a, const CombatantStats* b) {
            const double rate_a = dps_figure(*a, dps_metric);
            const double rate_b = dps_figure(*b, dps_metric);
            if (rate_a != rate_b) return rate_a > rate_b;
            return a->total_damage > b->total_damage;
        });
    }

    return list;
}

} // namespace hub::meter

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "imgui.h"
#include "imgui_internal.h"

namespace hub::meter {

namespace {

namespace palette = hub::common::ui::overlay_colors;
namespace chrome = hub::common::ui::chrome;
using hub::common::ui::rgba;

/// Figures beside the one a row is ranked by, and a row's rank.
constexpr float kSecondaryAlpha = 0.86f;
constexpr float kRankAlpha = 0.60f;
/// How far a job's colour is lifted for its label, which sits on its own bar.
constexpr float kJobLabelLift = 0.45f;
/// A row bar's opacity: low enough that the figures over it stay the brightest thing.
constexpr float kRowBarAlpha = 0.38f;
/// The local player's stripe, and the room the rank leaves for it.
constexpr float kStripeWidth = 3.0f;
constexpr float kRankInset = 5.0f;

inline hub::common::ui::CombatantStyle combatant_style(const CombatantStats& c) {
    return hub::common::ui::combatant_style(c.job, c.actor_type == ActorType::LimitBreak);
}

/// The Job column: three letters in the job's color, with the full job name on
/// hover since the abbreviation is all the width allows.
void render_job_cell(const CombatantStats& c, const hub::common::ui::CombatantStyle& style) {
    ImGui::TextColored(rgba(chrome::lighten(chrome::from_packed(style.rgb), kJobLabelLift)), "%.*s",
                       static_cast<int>(style.label.size()), style.label.data());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", c.actor_type == ActorType::LimitBreak
                                    ? "Limit Break"
                                    : std::string(to_string(c.job)).c_str());
    }
}

/// A resizable table takes its TableSetupColumn widths only when it is created,
/// so a scale change resets it to be laid out again at the new widths.
void relayout_on_scale_change(const char* table_id, float scale, float& laid_out_at) {
    if (laid_out_at == scale) return;
    if (ImGuiTable* table = ImGui::TableFindByID(ImGui::GetID(table_id))) {
        table->IsResetAllRequest = true;
    }
    laid_out_at = scale;
}

void format_number(char* buf, size_t n, uint64_t val) {
    if (val >= 1'000'000'000) {
        std::snprintf(buf, n, "%.2fB", static_cast<double>(val) / 1'000'000'000.0);
    } else if (val >= 1'000'000) {
        std::snprintf(buf, n, "%.2fM", static_cast<double>(val) / 1'000'000.0);
    } else if (val >= 1'000) {
        std::snprintf(buf, n, "%.1fk", static_cast<double>(val) / 1'000.0);
    } else {
        std::snprintf(buf, n, "%llu", static_cast<unsigned long long>(val));
    }
}

void format_rate(char* buf, size_t n, double val) {
    if (val >= 1'000'000.0) {
        std::snprintf(buf, n, "%.2fM", val / 1'000'000.0);
    } else if (val >= 1'000.0) {
        std::snprintf(buf, n, "%.1fk", val / 1'000.0);
    } else {
        std::snprintf(buf, n, "%.1f", val);
    }
}

/// A figure a row is not ranked by, a step dimmer than the one it is.
void text_secondary(const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, rgba(palette::Text, kSecondaryAlpha));
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

void text_number(uint64_t val) {
    char buf[32];
    format_number(buf, sizeof(buf), val);
    text_secondary(buf);
}

void text_percent(double val) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f%%", val);
    text_secondary(buf);
}

/// The figure a row is ranked by, in `bold`.
void text_rate(double val, ImFont* bold) {
    char buf[32];
    format_rate(buf, sizeof(buf), val);
    const chrome::FontScope font(bold);
    ImGui::TextUnformatted(buf);
}

/// Centres the current line's text within a row taller than one line.
void center_in_row(float row_h) {
    const float slack = row_h - ImGui::GetTextLineHeight();
    if (slack > 1.0f) {
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + slack * 0.5f);
    }
}

/// Moves to the current row's next cell and centres its text in the row.
void next_cell(int& col, float row_h) {
    ImGui::TableSetColumnIndex(col++);
    center_in_row(row_h);
}

/// Opens a ranked table with the columns every one leads with: rank, job and name.
/// The caller sets up its own after them, then calls render_ranked_headers().
bool begin_ranked_table(const char* table_id, int columns, float scale, float& laid_out_at) {
    // No row tint and no column lines: each row's bar is what separates it.
    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_SizingStretchSame;

    relayout_on_scale_change(table_id, scale, laid_out_at);
    if (!ImGui::BeginTable(table_id, columns, flags, ImVec2(0, 0))) return false;
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 24.0f * scale);
    ImGui::TableSetupColumn("Job", ImGuiTableColumnFlags_WidthFixed, 36.0f * scale);
    ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

/// Freezes the header row and draws its captions dim, with no band behind them.
void render_ranked_headers() {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::PushStyleColor(ImGuiCol_Text, rgba(palette::Muted));
    ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, rgba(palette::Black, 0.0f));
    ImGui::TableHeadersRow();
    ImGui::PopStyleColor(2);
}

/// A row's rank, job and name cells, leaving `col` on the next one. The local
/// player's name is in `bold`.
void render_ranked_cells(const CombatantStats& player, const hub::common::ui::CombatantStyle& style,
                         int rank, float row_h, float scale, ImFont* bold, int& col) {
    next_cell(col, row_h);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + kRankInset * scale);
    ImGui::TextColored(rgba(palette::Text, kRankAlpha), "%d", rank);

    next_cell(col, row_h);
    render_job_cell(player, style);

    next_cell(col, row_h);
    const chrome::FontScope font(player.is_local_player ? bold : nullptr);
    ImGui::TextUnformatted(player.name.c_str());
}

} // namespace

float CombatOverlay::row_height(float scale) const {
    // Rows track the rendered text, padding included, so they stay proportionate
    // as scale changes.
    return ImGui::GetTextLineHeightWithSpacing() + 6.0f * scale;
}

void CombatOverlay::begin_ranked_row(float fraction, uint32_t job_rgb, bool local_player, float row_h, float scale) {
    ImGui::TableNextRow(0, row_h);
    ImGuiTable* table = ImGui::GetCurrentTable();
    if (table == nullptr) return;

    // Drawn before any cell's text, so under it, from the table's geometry: the
    // row spans every column. Column 0's clip is widened to the table, below the
    // frozen header.
    ImGui::TableSetColumnIndex(0);
    const float gap = std::max(1.0f, std::round(scale));
    const ImVec2 row_min(table->WorkRect.Min.x, table->RowPosY1 + gap);
    const ImVec2 row_max(table->WorkRect.Max.x, table->RowPosY1 + row_h - gap);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushClipRect(ImVec2(table->InnerClipRect.Min.x, table->BgClipRect.Min.y), table->InnerClipRect.Max, false);
    if (m_show_progress_bars.load()) {
        chrome::BarStyle bar;
        bar.alpha = kRowBarAlpha;
        chrome::draw_bar(dl, row_min, row_max, fraction, chrome::from_packed(job_rgb), scale, bar);
    }
    if (local_player) {
        dl->AddRectFilled(row_min, ImVec2(row_min.x + kStripeWidth * scale, row_max.y), chrome::u32(palette::AccentBright),
                          kStripeWidth * scale, ImDrawFlags_RoundCornersLeft);
    }
    ImGui::PopClipRect();
}

void CombatOverlay::render_top_bar(const EncounterSummary& current, float scale) {
    // Breakpoints grow with the text they make room for.
    const float avail_w = ImGui::GetWindowWidth();
    const bool compact = avail_w < 520.0f * scale;
    const bool roomy = avail_w >= 700.0f * scale;
    const bool healing = (m_metric.load() == MeterMetric::Healing);

    ImFont* bold = font_for_scale(scale, /*bold_base=*/true).font;
    const float gap = 9.0f * scale;

    // Status light: whether the numbers below are still moving. Read from the cached
    // summary, since asking the engine would take its lock on every frame.
    const bool live = current.state == EncounterState::InCombat;
    const auto status_color = live ? palette::Live : palette::Muted;
    chrome::dot(status_color, scale);
    ImGui::SameLine();
    {
        const chrome::FontScope font(bold);
        ImGui::TextColored(rgba(status_color), live ? "LIVE" : "IDLE");
    }

    ImGui::SameLine(0.0f, gap);
    const uint32_t total_sec = static_cast<uint32_t>(current.duration_seconds);
    ImGui::TextColored(rgba(palette::Timer), "%02u:%02u", total_sec / 60, total_sec % 60);

    ImGui::SameLine(0.0f, gap);
    char rate_buf[32];
    {
        const chrome::FontScope font(bold);
        if (healing) {
            format_rate(rate_buf, sizeof(rate_buf), current.total_hps);
            ImGui::TextColored(rgba(palette::Hps), "%s HPS", rate_buf);
        } else {
            format_rate(rate_buf, sizeof(rate_buf), current.total_dps);
            ImGui::TextColored(rgba(palette::Dps), "%s DPS", rate_buf);
        }
    }

    // Progressive disclosure: total damage next, zone name only when there is room.
    if (!compact) {
        ImGui::SameLine(0.0f, gap);
        char dmg_buf[32];
        const uint64_t total = healing ? current.total_effective_healing : current.total_damage;
        format_number(dmg_buf, sizeof(dmg_buf), total);
        ImGui::TextColored(rgba(palette::TotalText), "%s", dmg_buf);
    }
    if (roomy) {
        const std::string zone = zone_label(current.zone_id, current.zone_name);
        if (!zone.empty()) {
            ImGui::SameLine(0.0f, gap);
            ImGui::TextColored(rgba(palette::Muted), "%s", zone.c_str());
        }
    }
}

void CombatOverlay::render_damage_table(const EncounterSummary& summary, float scale) {
    const DpsMetric dps_metric = m_dps_metric.load();
    const auto players = sorted_combatants(summary, m_party_only.load(), /*by_healing=*/false,
                                           m_hide_inactive.load(), dps_metric);
    const double top_dps = players.empty() ? 1.0 : std::max(dps_figure(*players.front(), dps_metric), 1.0);
    const std::string dps_label(to_string(dps_metric));

    const bool col_share = m_show_col_share.load();
    const bool col_crit = m_show_col_crit.load();
    const bool col_dh = m_show_col_dh.load();
    const bool col_cdh = m_show_col_cdh.load();
    const int columns = 5 + (col_share ? 1 : 0) + (col_crit ? 1 : 0) + (col_dh ? 1 : 0) + (col_cdh ? 1 : 0);

    if (begin_ranked_table("##DmgTable", columns, scale, m_damage_layout_scale)) {
        ImGui::TableSetupColumn(dps_label.c_str(), ImGuiTableColumnFlags_WidthFixed, 70.0f * scale);
        ImGui::TableSetupColumn("Damage", ImGuiTableColumnFlags_WidthFixed, 70.0f * scale);
        if (col_share) ImGui::TableSetupColumn("Share", ImGuiTableColumnFlags_WidthFixed, 56.0f * scale);
        if (col_crit) ImGui::TableSetupColumn("Crit", ImGuiTableColumnFlags_WidthFixed, 50.0f * scale);
        if (col_dh) ImGui::TableSetupColumn("DH", ImGuiTableColumnFlags_WidthFixed, 46.0f * scale);
        if (col_cdh) ImGui::TableSetupColumn("CDH", ImGuiTableColumnFlags_WidthFixed, 48.0f * scale);
        render_ranked_headers();

        ImFont* bold = font_for_scale(scale, /*bold_base=*/true).font;
        const float row_h = row_height(scale);
        int rank = 1;
        for (const CombatantStats* player : players) {
            int col = 0;
            const auto style = combatant_style(*player);
            begin_ranked_row(static_cast<float>(dps_figure(*player, dps_metric) / top_dps), style.rgb,
                             player->is_local_player, row_h, scale);
            render_ranked_cells(*player, style, rank++, row_h, scale, bold, col);

            next_cell(col, row_h);
            text_rate(dps_figure(*player, dps_metric), bold);

            next_cell(col, row_h);
            text_number(player->total_damage);

            if (col_share) {
                next_cell(col, row_h);
                text_percent(player->damage_share_pct);
            }
            if (col_crit) {
                next_cell(col, row_h);
                text_percent(player->hits.crit_rate());
            }
            if (col_dh) {
                next_cell(col, row_h);
                text_percent(player->hits.dh_rate());
            }
            if (col_cdh) {
                next_cell(col, row_h);
                text_percent(player->hits.cdh_rate());
            }
        }
        ImGui::EndTable();
    }
}

void CombatOverlay::render_healing_table(const EncounterSummary& summary, float scale) {
    const auto healers = sorted_combatants(summary, m_party_only.load(), /*by_healing=*/true, m_hide_inactive.load());
    const double top_hps = healers.empty() ? 1.0 : std::max(healers.front()->hps, 1.0);

    const bool col_crit = m_show_col_crit.load();
    const int columns = 7 + (col_crit ? 1 : 0);

    if (begin_ranked_table("##HealTable", columns, scale, m_healing_layout_scale)) {
        ImGui::TableSetupColumn("HPS", ImGuiTableColumnFlags_WidthFixed, 70.0f * scale);
        ImGui::TableSetupColumn("Heal", ImGuiTableColumnFlags_WidthFixed, 70.0f * scale);
        ImGui::TableSetupColumn("Overheal", ImGuiTableColumnFlags_WidthFixed, 70.0f * scale);
        ImGui::TableSetupColumn("OH%", ImGuiTableColumnFlags_WidthFixed, 56.0f * scale);
        if (col_crit) ImGui::TableSetupColumn("Crit", ImGuiTableColumnFlags_WidthFixed, 50.0f * scale);
        render_ranked_headers();

        ImFont* bold = font_for_scale(scale, /*bold_base=*/true).font;
        const float row_h = row_height(scale);
        int rank = 1;
        for (const CombatantStats* player : healers) {
            int col = 0;
            const auto style = combatant_style(*player);
            begin_ranked_row(static_cast<float>(player->hps / top_hps), style.rgb, player->is_local_player,
                             row_h, scale);
            render_ranked_cells(*player, style, rank++, row_h, scale, bold, col);

            next_cell(col, row_h);
            text_rate(player->hps, bold);

            next_cell(col, row_h);
            text_number(player->effective_healing);

            // Raw overhealed amount, not just the ratio: the absolute number is
            // what tells you how much of a cooldown was wasted.
            next_cell(col, row_h);
            const uint64_t overheal = (player->total_healing > player->effective_healing)
                                          ? player->total_healing - player->effective_healing
                                          : 0;
            text_number(overheal);

            next_cell(col, row_h);
            const double oh_pct = player->overheal_pct();
            if (oh_pct > 50.0) {
                ImGui::TextColored(rgba(palette::HighOverheal), "%.1f%%", oh_pct);
            } else {
                text_percent(oh_pct);
            }

            if (col_crit) {
                next_cell(col, row_h);
                // Heals are counted apart from damage hits.
                text_percent(player->heal_hit_counts.crit_rate());
            }
        }
        ImGui::EndTable();
    }
}

void CombatOverlay::render() {
    const float opacity = std::clamp(m_opacity.load(), 0.2f, 1.0f);
    const float scale = std::clamp(m_scale.load(), 0.7f, 2.0f);

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoFocusOnAppearing;

    if (m_click_through.load()) {
        flags |= ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
    }
    if (m_locked.load()) {
        flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
    }

    const ImGuiCond geom_cond = consume_geometry_restore() ? ImGuiCond_Always : ImGuiCond_FirstUseEver;
    const Rect geom = get_geometry();
    if (is_saved_position(geom)) {
        // Keep a title-bar-sized grab handle on screen so a window saved on a
        // since-unplugged monitor can still be dragged back.
        float x = geom.x;
        float y = geom.y;
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        if (display.x > 100.0f && display.y > 100.0f) {
            x = std::clamp(x, -geom.width + 120.0f, display.x - 120.0f);
            y = std::clamp(y, 0.0f, display.y - 40.0f);
        }
        ImGui::SetNextWindowPos(ImVec2(x, y), geom_cond);
    }
    ImGui::SetNextWindowSize(ImVec2(geom.width, geom.height), geom_cond);

    // The border is full while the meter can be moved, so an empty one can be found.
    chrome::Frame frame;
    frame.scale = scale;
    frame.opacity = opacity;
    frame.border_alpha = m_locked.load() ? std::min(1.0f, opacity + 0.2f) : 1.0f;
    const chrome::FrameScope frame_scope(frame);

    const ScaledFont scaled_font = font_for_scale(scale, /*bold_base=*/false);
    const bool push_font = (scaled_font.font != nullptr && scaled_font.font != ImGui::GetFont());
    if (push_font) {
        ImGui::PushFont(scaled_font.font);
    }

    if (ImGui::Begin(overlay_id(), nullptr, flags)) {
        ImGui::SetWindowFontScale(scaled_font.residual);
        const ImVec2 cur_pos = ImGui::GetWindowPos();
        const ImVec2 cur_size = ImGui::GetWindowSize();
        store_window_geometry(Rect{cur_pos.x, cur_pos.y, cur_size.x, cur_size.y});

        if (m_engine) {
            const auto now = std::chrono::steady_clock::now();
            const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_refresh);
            if (age.count() >= static_cast<long long>(m_refresh_interval_ms.load())) {
                // The tables read only the rankings, not the per-action maps or detail rows.
                m_cached_summary = m_engine->current_rankings();
                m_last_refresh = now;
            }
        }

        render_top_bar(m_cached_summary, scale);
        chrome::hairline();

        if (m_metric.load() == MeterMetric::Healing) {
            render_healing_table(m_cached_summary, scale);
        } else {
            render_damage_table(m_cached_summary, scale);
        }
    }
    ImGui::End();

    if (push_font) {
        ImGui::PopFont();
    }
}

} // namespace hub::meter

#else // !_WIN32 - Cross-platform mock implementation

namespace hub::meter {

void CombatOverlay::render() {}

} // namespace hub::meter

#endif
