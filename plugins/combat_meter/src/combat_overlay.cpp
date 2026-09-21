#include "meter/combat_overlay.hpp"
#include "payload/overlay_host.hpp"
#include "common/ui/job_style.hpp"
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
    const EncounterSummary& summary, bool party_only, bool by_healing, bool hide_inactive
) {
    std::vector<const CombatantStats*> list;
    list.reserve(summary.combatants.size());

    for (const auto& c : summary.combatants) {
        if (c.is_pet) continue;  // Merged into the owner's totals.
        if (party_only && !c.is_friendly()) continue;
        if (c.total_damage == 0 && c.total_healing == 0 && c.damage_taken == 0) continue;
        if (hide_inactive) {
            const uint64_t contribution = by_healing ? c.effective_healing : c.total_damage;
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
        std::sort(list.begin(), list.end(), [](const CombatantStats* a, const CombatantStats* b) {
            if (a->dps != b->dps) return a->dps > b->dps;
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

/// Table headers render in bold; the body font is whatever the window pushed.
void push_header_font() {
    ImFont* bold = hub::payload::OverlayHost::instance().font_bold();
    if (bold != nullptr && bold != ImGui::GetFont()) {
        ImGui::PushFont(bold);
    }
}

void pop_header_font() {
    ImFont* bold = hub::payload::OverlayHost::instance().font_bold();
    if (bold != nullptr && bold == ImGui::GetFont()) {
        ImGui::PopFont();
    }
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

} // namespace

float CombatOverlay::row_height() const {
    // Rows track the rendered text so they stay proportionate as scale changes.
    return std::clamp(ImGui::GetTextLineHeightWithSpacing() + 6.0f, 24.0f, 34.0f);
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

void CombatOverlay::render_top_bar(const EncounterSummary& current) {
    const float avail_w = ImGui::GetWindowWidth();
    const bool compact = avail_w < 520.0f;
    const bool roomy = avail_w >= 700.0f;
    const bool healing = (m_metric.load() == MeterMetric::Healing);

    // Status pill: whether the numbers below are still moving.
    if (m_engine && m_engine->in_combat()) {
        ImGui::TextColored(ImVec4(0.95f, 0.30f, 0.35f, 1.0f), "LIVE");
    } else {
        ImGui::TextColored(ImVec4(0.55f, 0.60f, 0.70f, 1.0f), "IDLE");
    }

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    const uint32_t total_sec = static_cast<uint32_t>(current.duration_seconds);
    ImGui::TextColored(ImVec4(0.80f, 0.85f, 0.95f, 1.0f), "%02u:%02u", total_sec / 60, total_sec % 60);

    ImGui::SameLine();
    char rate_buf[32];
    if (healing) {
        format_rate(rate_buf, sizeof(rate_buf), current.total_hps);
        ImGui::TextColored(ImVec4(0.10f, 0.80f, 0.40f, 1.0f), "%s HPS", rate_buf);
    } else {
        format_rate(rate_buf, sizeof(rate_buf), current.total_dps);
        ImGui::TextColored(ImVec4(0.96f, 0.50f, 0.20f, 1.0f), "%s DPS", rate_buf);
    }

    // Progressive disclosure: total damage next, zone name only when there is room.
    if (!compact) {
        ImGui::SameLine();
        char dmg_buf[32];
        const uint64_t total = healing ? current.total_effective_healing : current.total_damage;
        format_number(dmg_buf, sizeof(dmg_buf), total);
        ImGui::TextColored(ImVec4(0.70f, 0.75f, 0.85f, 1.0f), "%s", dmg_buf);
    }
    if (roomy) {
        const std::string zone = zone_label(current.zone_id, current.zone_name);
        if (!zone.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("| %s", zone.c_str());
        }
    }
}

void CombatOverlay::render_damage_table(const EncounterSummary& summary) {
    const auto players = sorted_combatants(summary, m_party_only.load(), /*by_healing=*/false, m_hide_inactive.load());
    const double top_dps = players.empty() ? 1.0 : std::max(players.front()->dps, 1.0);

    const bool col_share = m_show_col_share.load();
    const bool col_crit = m_show_col_crit.load();
    const bool col_dh = m_show_col_dh.load();
    const bool col_cdh = m_show_col_cdh.load();
    const int columns = 5 + (col_share ? 1 : 0) + (col_crit ? 1 : 0) + (col_dh ? 1 : 0) + (col_cdh ? 1 : 0);

    ImGuiTableFlags flags = ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_Resizable |
                            ImGuiTableFlags_SizingStretchSame;

    if (ImGui::BeginTable("##DmgTable", columns, flags, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 22.0f);
        ImGui::TableSetupColumn("Job", ImGuiTableColumnFlags_WidthFixed, 36.0f);
        ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("DPS", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Damage", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        if (col_share) ImGui::TableSetupColumn("Share", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        if (col_crit) ImGui::TableSetupColumn("Crit", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        if (col_dh) ImGui::TableSetupColumn("DH", ImGuiTableColumnFlags_WidthFixed, 46.0f);
        if (col_cdh) ImGui::TableSetupColumn("CDH", ImGuiTableColumnFlags_WidthFixed, 48.0f);
        ImGui::TableSetupScrollFreeze(0, 1);

        push_header_font();
        ImGui::TableHeadersRow();
        pop_header_font();

        const float row_h = row_height();
        int rank = 1;
        for (const CombatantStats* player : players) {
            ImGui::TableNextRow(0, row_h);
            int col = 0;

            ImGui::TableSetColumnIndex(col++);
            center_in_row(row_h);
            ImGui::TextDisabled("%d", rank++);

            const auto style = combatant_style(*player);

            ImGui::TableSetColumnIndex(col++);
            center_in_row(row_h);
            render_job_cell(*player, style);

            ImGui::TableSetColumnIndex(col++);
            center_in_row(row_h);
            ImGui::TextUnformatted(player->name.c_str());

            ImGui::TableSetColumnIndex(col++);
            center_in_row(row_h);
            text_rate(player->dps);

            ImGui::TableSetColumnIndex(col++);
            center_in_row(row_h);
            text_number(player->total_damage);

            if (col_share) {
                ImGui::TableSetColumnIndex(col++);
                center_in_row(row_h);
                ImGui::Text("%.1f%%", player->damage_share_pct);
            }
            if (col_crit) {
                ImGui::TableSetColumnIndex(col++);
                center_in_row(row_h);
                ImGui::Text("%.1f%%", player->hits.crit_rate());
            }
            if (col_dh) {
                ImGui::TableSetColumnIndex(col++);
                center_in_row(row_h);
                ImGui::Text("%.1f%%", player->hits.dh_rate());
            }
            if (col_cdh) {
                ImGui::TableSetColumnIndex(col++);
                center_in_row(row_h);
                ImGui::Text("%.1f%%", player->hits.cdh_rate());
            }

            render_row_progress_bar(
                static_cast<float>(player->dps / top_dps),
                style_color(style, 48)
            );
        }
        ImGui::EndTable();
    }
}

void CombatOverlay::render_healing_table(const EncounterSummary& summary) {
    const auto healers = sorted_combatants(summary, m_party_only.load(), /*by_healing=*/true, m_hide_inactive.load());
    const double top_hps = healers.empty() ? 1.0 : std::max(healers.front()->hps, 1.0);

    const bool col_crit = m_show_col_crit.load();
    const int columns = 7 + (col_crit ? 1 : 0);

    ImGuiTableFlags flags = ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_Resizable |
                            ImGuiTableFlags_SizingStretchSame;

    if (ImGui::BeginTable("##HealTable", columns, flags, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 22.0f);
        ImGui::TableSetupColumn("Job", ImGuiTableColumnFlags_WidthFixed, 36.0f);
        ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("HPS", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Heal", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("Overheal", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("OH%", ImGuiTableColumnFlags_WidthFixed, 56.0f);
        if (col_crit) ImGui::TableSetupColumn("Crit", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupScrollFreeze(0, 1);

        push_header_font();
        ImGui::TableHeadersRow();
        pop_header_font();

        const float row_h = row_height();
        int rank = 1;
        for (const CombatantStats* player : healers) {
            ImGui::TableNextRow(0, row_h);
            int col = 0;

            ImGui::TableSetColumnIndex(col++);
            center_in_row(row_h);
            ImGui::TextDisabled("%d", rank++);

            const auto style = combatant_style(*player);

            ImGui::TableSetColumnIndex(col++);
            center_in_row(row_h);
            render_job_cell(*player, style);

            ImGui::TableSetColumnIndex(col++);
            center_in_row(row_h);
            ImGui::TextUnformatted(player->name.c_str());

            ImGui::TableSetColumnIndex(col++);
            center_in_row(row_h);
            text_rate(player->hps);

            ImGui::TableSetColumnIndex(col++);
            center_in_row(row_h);
            text_number(player->effective_healing);

            // Raw overhealed amount, not just the ratio: the absolute number is
            // what tells you how much of a cooldown was wasted.
            ImGui::TableSetColumnIndex(col++);
            center_in_row(row_h);
            const uint64_t overheal = (player->total_healing > player->effective_healing)
                                          ? player->total_healing - player->effective_healing
                                          : 0;
            text_number(overheal);

            ImGui::TableSetColumnIndex(col++);
            center_in_row(row_h);
            const double oh_pct = player->overheal_pct();
            if (oh_pct > 50.0) {
                ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "%.1f%%", oh_pct);
            } else {
                ImGui::Text("%.1f%%", oh_pct);
            }

            if (col_crit) {
                ImGui::TableSetColumnIndex(col++);
                center_in_row(row_h);
                ImGui::Text("%.1f%%", player->hits.crit_rate());
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
    if (has_saved_position()) {
        // Keep a title-bar-sized grab handle on screen so a window saved on a
        // since-unplugged monitor can still be dragged back.
        float x = m_pos_x;
        float y = m_pos_y;
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        if (display.x > 100.0f && display.y > 100.0f) {
            x = std::clamp(x, -m_width + 120.0f, display.x - 120.0f);
            y = std::clamp(y, 0.0f, display.y - 40.0f);
        }
        ImGui::SetNextWindowPos(ImVec2(x, y), geom_cond);
    }
    ImGui::SetNextWindowSize(ImVec2(m_width, m_height), geom_cond);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 10.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.09f, 0.12f, opacity));

    const auto scaled_font = hub::payload::OverlayHost::instance().font_for_scale(scale, /*bold_base=*/false);
    const bool push_font = (scaled_font.font != nullptr && scaled_font.font != ImGui::GetFont());
    if (push_font) {
        ImGui::PushFont(scaled_font.font);
    }

    if (ImGui::Begin(overlay_id(), nullptr, flags)) {
        ImGui::SetWindowFontScale(scaled_font.residual);
        const ImVec2 cur_pos = ImGui::GetWindowPos();
        const ImVec2 cur_size = ImGui::GetWindowSize();
        m_pos_x = cur_pos.x;
        m_pos_y = cur_pos.y;
        m_width = cur_size.x;
        m_height = cur_size.y;

        if (m_engine) {
            const auto now = std::chrono::steady_clock::now();
            const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_refresh);
            if (age.count() >= static_cast<long long>(m_refresh_interval_ms.load())) {
                m_cached_summary = m_engine->current_summary();
                m_last_refresh = now;
            }
        }

        render_top_bar(m_cached_summary);
        ImGui::Separator();

        if (m_metric.load() == MeterMetric::Healing) {
            render_healing_table(m_cached_summary);
        } else {
            render_damage_table(m_cached_summary);
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
