#include "meter/combat_overlay.hpp"
#include "common/ui/job_style.hpp"
#include "common/ui/overlay_palette.hpp"
#include <algorithm>
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
using hub::common::ui::rgba;

/// The shared table already packs in IM_COL32 channel order; only alpha differs
/// between the row tint and the name text.
inline uint32_t style_color(const hub::common::ui::CombatantStyle& style, uint32_t alpha) {
    return (style.rgb & 0x00FFFFFFu) | (alpha << 24);
}

inline hub::common::ui::CombatantStyle combatant_style(const CombatantStats& c) {
    return hub::common::ui::combatant_style(c.job, c.actor_type == ActorType::LimitBreak);
}

/// The Job column: three letters in the job's color, with the full job name on
/// hover since the abbreviation is all the width allows.
void render_job_cell(const CombatantStats& c, const hub::common::ui::CombatantStyle& style) {
    ImGui::TextColored(ImColor(style_color(style, 255)), "%.*s",
                       static_cast<int>(style.label.size()), style.label.data());
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", c.actor_type == ActorType::LimitBreak
                                    ? "Limit Break"
                                    : std::string(to_string(c.job)).c_str());
    }
}

/// Table headers render in bold at the rows' own size tier (`bold`, the bold
/// font for the meter's scale); the base bold font left them smaller than the
/// rows once the meter was scaled up. Returns whether it pushed, since above the
/// base tier the rows are bold already.
bool push_header_font(ImFont* bold) {
    if (bold == nullptr || bold == ImGui::GetFont()) return false;
    ImGui::PushFont(bold);
    return true;
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

void text_number(uint64_t val) {
    char buf[32];
    format_number(buf, sizeof(buf), val);
    ImGui::TextUnformatted(buf);
}

void text_rate(double val) {
    char buf[32];
    format_rate(buf, sizeof(buf), val);
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
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_SizingStretchSame;

    relayout_on_scale_change(table_id, scale, laid_out_at);
    if (!ImGui::BeginTable(table_id, columns, flags, ImVec2(0, 0))) return false;
    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 22.0f * scale);
    ImGui::TableSetupColumn("Job", ImGuiTableColumnFlags_WidthFixed, 36.0f * scale);
    ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch);
    return true;
}

/// Freezes the header row and draws it in the header font.
void render_ranked_headers(ImFont* header_font_for_scale) {
    ImGui::TableSetupScrollFreeze(0, 1);
    const bool header_font = push_header_font(header_font_for_scale);
    ImGui::TableHeadersRow();
    if (header_font) ImGui::PopFont();
}

/// Starts a row with its rank, job and name cells, leaving `col` on the next one.
/// Returns the row's style for its progress bar.
hub::common::ui::CombatantStyle begin_ranked_row(const CombatantStats& player, int rank, float row_h, int& col) {
    ImGui::TableNextRow(0, row_h);

    next_cell(col, row_h);
    ImGui::TextDisabled("%d", rank);

    const auto style = combatant_style(player);

    next_cell(col, row_h);
    render_job_cell(player, style);

    next_cell(col, row_h);
    ImGui::TextUnformatted(player.name.c_str());
    return style;
}

} // namespace

float CombatOverlay::row_height(float scale) const {
    // Rows track the rendered text, padding included, so they stay proportionate
    // as scale changes.
    return ImGui::GetTextLineHeightWithSpacing() + 6.0f * scale;
}



void CombatOverlay::render_row_progress_bar(float fraction, uint32_t color_u32) {
    if (!m_show_progress_bars.load()) return;
    fraction = std::clamp(fraction, 0.0f, 1.0f);
    if (fraction <= 0.0f) return;

    // Spans the whole row, so it has to come from the table's geometry. The
    // last-item rect belongs to whatever cell was submitted most recently.
    ImGuiContext* g = ImGui::GetCurrentContext();
    if (!g) return;
    ImGuiTable* table = g->CurrentTable;
    if (!table) return;

    const ImVec2 row_min(table->WorkRect.Min.x, table->RowPosY1);
    const float row_w = table->WorkRect.Max.x - table->WorkRect.Min.x;
    const ImVec2 row_max(row_min.x + row_w * fraction, table->RowPosY2);

    ImGui::GetWindowDrawList()->AddRectFilled(row_min, row_max, color_u32, 0.0f);
}

void CombatOverlay::render_top_bar(const EncounterSummary& current, float scale) {
    // Breakpoints grow with the text they make room for.
    const float avail_w = ImGui::GetWindowWidth();
    const bool compact = avail_w < 520.0f * scale;
    const bool roomy = avail_w >= 700.0f * scale;
    const bool healing = (m_metric.load() == MeterMetric::Healing);

    // Status pill: whether the numbers below are still moving. Read from the cached
    // summary, since asking the engine would take its lock on every frame.
    if (current.state == EncounterState::InCombat) {
        ImGui::TextColored(rgba(palette::Live), "LIVE");
    } else {
        ImGui::TextColored(rgba(palette::Muted), "IDLE");
    }

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    const uint32_t total_sec = static_cast<uint32_t>(current.duration_seconds);
    ImGui::TextColored(rgba(palette::Timer), "%02u:%02u", total_sec / 60, total_sec % 60);

    ImGui::SameLine();
    char rate_buf[32];
    if (healing) {
        format_rate(rate_buf, sizeof(rate_buf), current.total_hps);
        ImGui::TextColored(rgba(palette::Hps), "%s HPS", rate_buf);
    } else {
        format_rate(rate_buf, sizeof(rate_buf), current.total_dps);
        ImGui::TextColored(rgba(palette::Dps), "%s DPS", rate_buf);
    }

    // Progressive disclosure: total damage next, zone name only when there is room.
    if (!compact) {
        ImGui::SameLine();
        char dmg_buf[32];
        const uint64_t total = healing ? current.total_effective_healing : current.total_damage;
        format_number(dmg_buf, sizeof(dmg_buf), total);
        ImGui::TextColored(rgba(palette::TotalText), "%s", dmg_buf);
    }
    if (roomy) {
        const std::string zone = zone_label(current.zone_id, current.zone_name);
        if (!zone.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("| %s", zone.c_str());
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
        render_ranked_headers(font_for_scale(scale, /*bold_base=*/true).font);

        const float row_h = row_height(scale);
        int rank = 1;
        for (const CombatantStats* player : players) {
            int col = 0;
            const auto style = begin_ranked_row(*player, rank++, row_h, col);

            next_cell(col, row_h);
            text_rate(dps_figure(*player, dps_metric));

            next_cell(col, row_h);
            text_number(player->total_damage);

            if (col_share) {
                next_cell(col, row_h);
                ImGui::Text("%.1f%%", player->damage_share_pct);
            }
            if (col_crit) {
                next_cell(col, row_h);
                ImGui::Text("%.1f%%", player->hits.crit_rate());
            }
            if (col_dh) {
                next_cell(col, row_h);
                ImGui::Text("%.1f%%", player->hits.dh_rate());
            }
            if (col_cdh) {
                next_cell(col, row_h);
                ImGui::Text("%.1f%%", player->hits.cdh_rate());
            }

            render_row_progress_bar(
                static_cast<float>(dps_figure(*player, dps_metric) / top_dps),
                style_color(style, 48)
            );
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
        render_ranked_headers(font_for_scale(scale, /*bold_base=*/true).font);

        const float row_h = row_height(scale);
        int rank = 1;
        for (const CombatantStats* player : healers) {
            int col = 0;
            const auto style = begin_ranked_row(*player, rank++, row_h, col);

            next_cell(col, row_h);
            text_rate(player->hps);

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
                ImGui::Text("%.1f%%", oh_pct);
            }

            if (col_crit) {
                next_cell(col, row_h);
                // Heals are counted apart from damage hits.
                ImGui::Text("%.1f%%", player->heal_hit_counts.crit_rate());
            }

            render_row_progress_bar(
                static_cast<float>(player->hps / top_hps),
                style_color(style, 48)
            );
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

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 10.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, rgba(palette::Background, opacity));

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
        ImGui::Separator();

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
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

} // namespace hub::meter

#else // !_WIN32 - Cross-platform mock implementation

namespace hub::meter {

void CombatOverlay::render() {}

} // namespace hub::meter

#endif
