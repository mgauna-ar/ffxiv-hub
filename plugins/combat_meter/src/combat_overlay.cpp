#include "meter/combat_overlay.hpp"
#include "payload/overlay_host.hpp"
#include <algorithm>
#include <cstdio>

namespace hub::meter {

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

inline uint32_t get_role_color(Job job) {
    Role role = job_to_role(job);
    switch (role) {
        case Role::Tank:
            return IM_COL32(59, 130, 246, 48);  // #3B82F6 Blue
        case Role::Healer:
            return IM_COL32(16, 185, 129, 48);  // #10B981 Green
        case Role::Melee:
            return IM_COL32(239, 68, 68, 48);   // #EF4444 Red
        case Role::Ranged:
            return IM_COL32(249, 115, 22, 48);  // #F97316 Orange
        case Role::Caster:
            return IM_COL32(168, 85, 247, 48);  // #A855F7 Purple
        default:
            return IM_COL32(100, 116, 139, 48); // Slate
    }
}

inline uint32_t get_job_accent_color(Job job) {
    Role role = job_to_role(job);
    switch (role) {
        case Role::Tank:
            return IM_COL32(96, 165, 250, 255);
        case Role::Healer:
            return IM_COL32(52, 211, 153, 255);
        case Role::Melee:
            return IM_COL32(248, 113, 113, 255);
        case Role::Ranged:
            return IM_COL32(251, 146, 60, 255);
        case Role::Caster:
            return IM_COL32(192, 132, 252, 255);
        default:
            return IM_COL32(148, 163, 184, 255);
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

const char* end_reason_label(EncounterEndReason reason) {
    switch (reason) {
        case EncounterEndReason::Wipe: return "Wipe";
        case EncounterEndReason::Inactivity: return "Timeout";
        case EncounterEndReason::ZoneChange: return "Zone";
        case EncounterEndReason::Manual: return "Clear";
        default: return "--";
    }
}

ImVec4 end_reason_color(EncounterEndReason reason) {
    switch (reason) {
        case EncounterEndReason::Wipe: return ImVec4(0.95f, 0.30f, 0.30f, 1.0f);
        case EncounterEndReason::Inactivity: return ImVec4(0.85f, 0.70f, 0.25f, 1.0f);
        case EncounterEndReason::Manual: return ImVec4(0.25f, 0.80f, 0.45f, 1.0f);
        default: return ImVec4(0.60f, 0.65f, 0.75f, 1.0f);
    }
}

} // namespace

float CombatOverlay::row_height() const {
    // Rows track the rendered text so they stay proportionate as scale changes.
    return std::clamp(ImGui::GetTextLineHeightWithSpacing() + 6.0f, 24.0f, 34.0f);
}

CombatOverlay::CombatOverlay(EncounterEngine* engine)
    : m_engine(engine) {
    set_geometry(Rect{-1.0f, -1.0f, 800.0f, 480.0f});
}

CombatOverlay::~CombatOverlay() = default;

bool CombatOverlay::should_render() const noexcept {
    if (!m_visible.load()) return false;
    if (m_auto_hide.load() && m_locked.load()) {
        if (m_engine && !m_engine->in_combat()) {
            return false;
        }
    }
    return true;
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

void CombatOverlay::render_padlock(float size) {
    const bool locked = m_locked.load();
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    if (ImGui::InvisibleButton("##LockToggle", ImVec2(size, size))) {
        m_locked.store(!locked);
    }
    const bool hovered = ImGui::IsItemHovered();
    if (hovered) {
        ImGui::SetTooltip("%s", locked ? "Locked (click to unlock)" : "Unlocked (click to lock)");
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = hovered ? IM_COL32(235, 240, 250, 255) : IM_COL32(160, 170, 190, 255);

    // Body sits on the lower half; the shackle arcs above it, tilted when open.
    const float body_w = size * 0.62f;
    const float body_h = size * 0.46f;
    const ImVec2 body_min(origin.x + (size - body_w) * 0.5f, origin.y + size - body_h - size * 0.08f);
    const ImVec2 body_max(body_min.x + body_w, body_min.y + body_h);
    dl->AddRectFilled(body_min, body_max, col, size * 0.10f);

    const float shackle_r = size * 0.22f;
    const ImVec2 shackle_c(origin.x + size * 0.5f + (locked ? 0.0f : size * 0.16f),
                           body_min.y - shackle_r * 0.35f);
    dl->PathArcTo(shackle_c, shackle_r, IM_PI, IM_PI * 2.0f, 12);
    dl->PathStroke(col, 0, std::max(1.0f, size * 0.10f));

    // Both legs reach the body when closed; the trailing one stays short when open.
    dl->AddLine(ImVec2(shackle_c.x - shackle_r, shackle_c.y),
                ImVec2(shackle_c.x - shackle_r, body_min.y),
                col, std::max(1.0f, size * 0.10f));
    if (locked) {
        dl->AddLine(ImVec2(shackle_c.x + shackle_r, shackle_c.y),
                    ImVec2(shackle_c.x + shackle_r, body_min.y),
                    col, std::max(1.0f, size * 0.10f));
    }
}

void CombatOverlay::render_top_bar(const EncounterSummary& current, bool viewing_history) {
    const float avail_w = ImGui::GetWindowWidth();
    const bool compact = avail_w < 520.0f;
    const bool roomy = avail_w >= 700.0f;

    bool is_damage = (m_active_tab == OverlayTab::Damage);
    bool is_healing = (m_active_tab == OverlayTab::Healing);
    bool is_history = (m_active_tab == OverlayTab::History);

    const ImVec4 active_bg(0.20f, 0.25f, 0.35f, 1.0f);

    if (is_damage) ImGui::PushStyleColor(ImGuiCol_Button, active_bg);
    if (ImGui::SmallButton(compact ? "D" : "DMG")) m_active_tab = OverlayTab::Damage;
    if (is_damage) ImGui::PopStyleColor();

    ImGui::SameLine();
    if (is_healing) ImGui::PushStyleColor(ImGuiCol_Button, active_bg);
    if (ImGui::SmallButton(compact ? "H" : "HEAL")) m_active_tab = OverlayTab::Healing;
    if (is_healing) ImGui::PopStyleColor();

    ImGui::SameLine();
    if (is_history) ImGui::PushStyleColor(ImGuiCol_Button, active_bg);
    if (ImGui::SmallButton(compact ? "P" : "HIST")) m_active_tab = OverlayTab::History;
    if (is_history) ImGui::PopStyleColor();

    // Status pill: which encounter the numbers below actually describe.
    ImGui::SameLine();
    if (viewing_history) {
        ImGui::TextColored(ImVec4(0.55f, 0.70f, 0.95f, 1.0f), "Pull #%d", m_selected_history_pull + 1);
    } else if (m_engine && m_engine->in_combat()) {
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
    if (m_active_tab == OverlayTab::Healing) {
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
        const uint64_t total = (m_active_tab == OverlayTab::Healing)
                                   ? current.total_effective_healing
                                   : current.total_damage;
        format_number(dmg_buf, sizeof(dmg_buf), total);
        ImGui::TextColored(ImVec4(0.70f, 0.75f, 0.85f, 1.0f), "%s", dmg_buf);
    }
    if (roomy && !current.zone_name.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("| %s", current.zone_name.c_str());
    }

    const float lock_size = ImGui::GetTextLineHeight() + 2.0f;
    if (ImGui::GetContentRegionAvail().x > lock_size + 12.0f) {
        ImGui::SameLine(ImGui::GetWindowWidth() - lock_size - 12.0f);
        render_padlock(lock_size);
    }
}

void CombatOverlay::render_damage_tab(const EncounterSummary& summary) {
    const auto players = sorted_combatants(summary, m_party_only.load(), /*by_healing=*/false, m_hide_inactive.load());
    const double top_dps = players.empty() ? 1.0 : std::max(players.front()->dps, 1.0);

    const bool col_share = m_show_col_share.load();
    const bool col_crit = m_show_col_crit.load();
    const bool col_dh = m_show_col_dh.load();
    const bool col_cdh = m_show_col_cdh.load();
    const int columns = 4 + (col_share ? 1 : 0) + (col_crit ? 1 : 0) + (col_dh ? 1 : 0) + (col_cdh ? 1 : 0);

    ImGuiTableFlags flags = ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_Resizable |
                            ImGuiTableFlags_SizingStretchSame;

    if (ImGui::BeginTable("##DmgTable", columns, flags, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 22.0f);
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

            ImGui::TableSetColumnIndex(col++);
            center_in_row(row_h);
            ImGui::TextColored(
                ImColor(get_job_accent_color(player->job)),
                "[%s] %s",
                std::string(job_abbreviation(player->job)).c_str(),
                player->name.c_str()
            );

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
                get_role_color(player->job)
            );
        }
        ImGui::EndTable();
    }
}

void CombatOverlay::render_healing_tab(const EncounterSummary& summary) {
    const auto healers = sorted_combatants(summary, m_party_only.load(), /*by_healing=*/true, m_hide_inactive.load());
    const double top_hps = healers.empty() ? 1.0 : std::max(healers.front()->hps, 1.0);

    const bool col_crit = m_show_col_crit.load();
    const int columns = 6 + (col_crit ? 1 : 0);

    ImGuiTableFlags flags = ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_Resizable |
                            ImGuiTableFlags_SizingStretchSame;

    if (ImGui::BeginTable("##HealTable", columns, flags, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 22.0f);
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

            ImGui::TableSetColumnIndex(col++);
            center_in_row(row_h);
            ImGui::TextColored(
                ImColor(get_job_accent_color(player->job)),
                "[%s] %s",
                std::string(job_abbreviation(player->job)).c_str(),
                player->name.c_str()
            );

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
                get_role_color(player->job)
            );
        }
        ImGui::EndTable();
    }
}

void CombatOverlay::render_history_tab() {
    if (!m_engine) {
        ImGui::TextDisabled("No encounter engine connected");
        return;
    }

    const auto& history = m_engine->pull_history();

    if (ImGui::SmallButton("Clear History")) {
        m_engine->clear_history();
        m_selected_history_pull = -1;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(Pulls stored: %zu)", history.size());

    if (history.empty()) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.7f, 1.0f), "No past encounters archived yet.");
        return;
    }

    ImGui::Separator();

    ImGuiTableFlags flags = ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_Resizable |
                            ImGuiTableFlags_SizingStretchSame;

    if (ImGui::BeginTable("##HistTable", 7, flags, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Pull", ImGuiTableColumnFlags_WidthFixed, 44.0f);
        ImGui::TableSetupColumn("Zone", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthFixed, 66.0f);
        ImGui::TableSetupColumn("Raid DPS", ImGuiTableColumnFlags_WidthFixed, 74.0f);
        ImGui::TableSetupColumn("Total Dmg", ImGuiTableColumnFlags_WidthFixed, 76.0f);
        ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthFixed, 62.0f);
        ImGui::TableSetupColumn("##View", ImGuiTableColumnFlags_WidthFixed, 52.0f);
        ImGui::TableSetupScrollFreeze(0, 1);

        push_header_font();
        ImGui::TableHeadersRow();
        pop_header_font();

        const float row_h = row_height();
        for (int i = static_cast<int>(history.size()) - 1; i >= 0; --i) {
            const auto& p = history[i];
            ImGui::PushID(i);
            ImGui::TableNextRow(0, row_h);

            ImGui::TableSetColumnIndex(0);
            center_in_row(row_h);
            const bool selected = (m_selected_history_pull == i);
            if (selected) {
                ImGui::TextColored(ImVec4(0.55f, 0.70f, 0.95f, 1.0f), "#%d", i + 1);
            } else {
                ImGui::Text("#%d", i + 1);
            }

            ImGui::TableSetColumnIndex(1);
            center_in_row(row_h);
            ImGui::TextUnformatted(p.zone_name.empty() ? "Unknown" : p.zone_name.c_str());

            ImGui::TableSetColumnIndex(2);
            center_in_row(row_h);
            const uint32_t s = static_cast<uint32_t>(p.duration_seconds);
            ImGui::Text("%02u:%02u", s / 60, s % 60);

            ImGui::TableSetColumnIndex(3);
            center_in_row(row_h);
            text_rate(p.total_dps);

            ImGui::TableSetColumnIndex(4);
            center_in_row(row_h);
            text_number(p.total_damage);

            ImGui::TableSetColumnIndex(5);
            center_in_row(row_h);
            ImGui::TextColored(end_reason_color(p.end_reason), "%s", end_reason_label(p.end_reason));

            ImGui::TableSetColumnIndex(6);
            center_in_row(row_h);
            if (ImGui::SmallButton("View")) {
                m_selected_history_pull = i;
                m_active_tab = OverlayTab::Damage;
            }

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void CombatOverlay::render() {
    if (!should_render()) return;

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

        static const std::vector<EncounterSummary> s_no_history;
        const auto& history = m_engine ? m_engine->pull_history() : s_no_history;
        const bool viewing_history =
            m_selected_history_pull >= 0 &&
            m_selected_history_pull < static_cast<int>(history.size());

        if (m_engine && !viewing_history) {
            const auto now = std::chrono::steady_clock::now();
            const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_refresh);
            if (age.count() >= static_cast<long long>(m_refresh_interval_ms.load())) {
                m_cached_summary = m_engine->current_summary();
                m_last_refresh = now;
            }
        }
        const EncounterSummary& shown =
            viewing_history ? history[m_selected_history_pull] : m_cached_summary;

        render_top_bar(shown, viewing_history);
        ImGui::Separator();

        switch (m_active_tab) {
            case OverlayTab::Damage:
                render_damage_tab(shown);
                break;
            case OverlayTab::Healing:
                render_healing_tab(shown);
                break;
            case OverlayTab::History:
                render_history_tab();
                break;
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

CombatOverlay::CombatOverlay(EncounterEngine* engine)
    : m_engine(engine) {
    set_geometry(Rect{-1.0f, -1.0f, 800.0f, 480.0f});
}

CombatOverlay::~CombatOverlay() = default;

bool CombatOverlay::should_render() const noexcept {
    if (!m_visible.load()) return false;
    if (m_auto_hide.load() && m_locked.load()) {
        if (m_engine && !m_engine->in_combat()) {
            return false;
        }
    }
    return true;
}

void CombatOverlay::render() {}

} // namespace hub::meter

#endif
