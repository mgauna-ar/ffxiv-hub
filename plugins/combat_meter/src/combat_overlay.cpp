#include "meter/combat_overlay.hpp"
#include <algorithm>
#include <cstdio>

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
            return IM_COL32(59, 130, 246, 75);  // #3B82F6 Blue
        case Role::Healer:
            return IM_COL32(16, 185, 129, 75);  // #10B981 Green
        case Role::Melee:
            return IM_COL32(239, 68, 68, 75);   // #EF4444 Red
        case Role::Ranged:
            return IM_COL32(249, 115, 22, 75);  // #F97316 Orange
        case Role::Caster:
            return IM_COL32(168, 85, 247, 75);  // #A855F7 Purple
        default:
            return IM_COL32(100, 116, 139, 75); // Slate
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

} // namespace

CombatOverlay::CombatOverlay(EncounterEngine* engine)
    : m_engine(engine) {
    set_geometry(Rect{50.0f, 100.0f, 420.0f, 220.0f});
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

    ImVec2 min_pos = ImGui::GetItemRectMin();
    ImVec2 max_pos = ImGui::GetItemRectMax();
    float bar_width = (max_pos.x - min_pos.x) * fraction;

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(
        min_pos,
        ImVec2(min_pos.x + bar_width, max_pos.y),
        color_u32,
        4.0f
    );
}

void CombatOverlay::render_top_bar(const EncounterSummary& current) {
    // 1. Navigation segmented buttons (Damage, Healing, History)
    bool is_damage = (m_active_tab == OverlayTab::Damage);
    bool is_healing = (m_active_tab == OverlayTab::Healing);
    bool is_history = (m_active_tab == OverlayTab::History);

    if (is_damage) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.25f, 0.35f, 1.0f));
    if (ImGui::SmallButton("DMG")) m_active_tab = OverlayTab::Damage;
    if (is_damage) ImGui::PopStyleColor();

    ImGui::SameLine();
    if (is_healing) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.25f, 0.35f, 1.0f));
    if (ImGui::SmallButton("HEAL")) m_active_tab = OverlayTab::Healing;
    if (is_healing) ImGui::PopStyleColor();

    ImGui::SameLine();
    if (is_history) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.25f, 0.35f, 1.0f));
    if (ImGui::SmallButton("HIST")) m_active_tab = OverlayTab::History;
    if (is_history) ImGui::PopStyleColor();

    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();

    // 2. Metrics summary (Time, DPS/HPS)
    const uint32_t total_sec = static_cast<uint32_t>(current.duration_seconds);
    const uint32_t mm = total_sec / 60;
    const uint32_t ss = total_sec % 60;

    ImGui::TextColored(ImVec4(0.80f, 0.85f, 0.95f, 1.0f), "%02u:%02u", mm, ss);
    ImGui::SameLine();

    if (m_active_tab == OverlayTab::Healing) {
        ImGui::TextColored(ImVec4(0.10f, 0.80f, 0.40f, 1.0f), "%.0f HPS", current.total_hps);
    } else {
        ImGui::TextColored(ImVec4(0.96f, 0.50f, 0.20f, 1.0f), "%.0f DPS", current.total_dps);
    }

    // 3. Right-anchored padlock toggle button
    float lock_btn_size = 20.0f;
    float avail = ImGui::GetContentRegionAvail().x;
    if (avail > lock_btn_size) {
        ImGui::SameLine(ImGui::GetWindowWidth() - lock_btn_size - 12.0f);
        const char* lock_icon = m_locked.load() ? "[L]" : "[U]";
        if (ImGui::SmallButton(lock_icon)) {
            m_locked.store(!m_locked.load());
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", m_locked.load() ? "Locked (Click to Unlock)" : "Unlocked (Click to Lock)");
        }
    }
}

void CombatOverlay::render_damage_tab(const EncounterSummary& summary) {
    auto players = summary.combatants;
    if (m_party_only.load()) {
        players.erase(
            std::remove_if(players.begin(), players.end(), [](const CombatantStats& c) {
                return !c.is_friendly();
            }),
            players.end()
        );
    }

    std::sort(players.begin(), players.end(), [](const CombatantStats& a, const CombatantStats& b) {
        return a.dps > b.dps;
    });

    const double top_dps = players.empty() ? 1.0 : std::max(players.front().dps, 1.0);

    ImGuiTableFlags flags = ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_ScrollY;

    if (ImGui::BeginTable("##DmgTable", 7, flags, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 18.0f);
        ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("DPS", ImGuiTableColumnFlags_WidthFixed, 65.0f);
        ImGui::TableSetupColumn("Damage", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("CRIT", ImGuiTableColumnFlags_WidthFixed, 42.0f);
        ImGui::TableSetupColumn("DH", ImGuiTableColumnFlags_WidthFixed, 38.0f);
        ImGui::TableSetupColumn("CDH", ImGuiTableColumnFlags_WidthFixed, 40.0f);
        ImGui::TableHeadersRow();

        int rank = 1;
        for (const auto& player : players) {
            ImGui::TableNextRow(0, 24.0f);

            // Background progress bar
            float fraction = static_cast<float>(player.dps / top_dps);
            render_row_progress_bar(fraction, get_role_color(player.job));

            // Rank
            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%d", rank++);

            // Player Name + Job
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(
                ImColor(get_job_accent_color(player.job)),
                "[%s] %s",
                std::string(job_abbreviation(player.job)).c_str(),
                player.name.c_str()
            );

            // DPS
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.1f", player.dps);

            // Total Damage
            ImGui::TableSetColumnIndex(3);
            if (player.total_damage >= 1'000'000) {
                ImGui::Text("%.2fM", player.total_damage / 1'000'000.0);
            } else if (player.total_damage >= 1'000) {
                ImGui::Text("%.1fk", player.total_damage / 1'000.0);
            } else {
                ImGui::Text("%llu", static_cast<unsigned long long>(player.total_damage));
            }

            // CRIT%
            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%.1f%%", player.hits.crit_rate());

            // DH%
            ImGui::TableSetColumnIndex(5);
            ImGui::Text("%.1f%%", player.hits.dh_rate());

            // CDH%
            ImGui::TableSetColumnIndex(6);
            ImGui::Text("%.1f%%", player.hits.cdh_rate());
        }
        ImGui::EndTable();
    }
}

void CombatOverlay::render_healing_tab(const EncounterSummary& summary) {
    auto healers = summary.combatants;
    if (m_party_only.load()) {
        healers.erase(
            std::remove_if(healers.begin(), healers.end(), [](const CombatantStats& c) {
                return !c.is_friendly();
            }),
            healers.end()
        );
    }

    std::sort(healers.begin(), healers.end(), [](const CombatantStats& a, const CombatantStats& b) {
        return a.hps > b.hps;
    });

    const double top_hps = healers.empty() ? 1.0 : std::max(healers.front().hps, 1.0);

    ImGuiTableFlags flags = ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_ScrollY;

    if (ImGui::BeginTable("##HealTable", 5, flags, ImVec2(0, 0))) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 18.0f);
        ImGui::TableSetupColumn("Player", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("HPS", ImGuiTableColumnFlags_WidthFixed, 65.0f);
        ImGui::TableSetupColumn("Heal", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableSetupColumn("Overheal", ImGuiTableColumnFlags_WidthFixed, 60.0f);
        ImGui::TableHeadersRow();

        int rank = 1;
        for (const auto& player : healers) {
            ImGui::TableNextRow(0, 24.0f);

            float fraction = static_cast<float>(player.hps / top_hps);
            render_row_progress_bar(fraction, get_role_color(player.job));

            ImGui::TableSetColumnIndex(0);
            ImGui::TextDisabled("%d", rank++);

            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(
                ImColor(get_job_accent_color(player.job)),
                "[%s] %s",
                std::string(job_abbreviation(player.job)).c_str(),
                player.name.c_str()
            );

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%.1f", player.hps);

            ImGui::TableSetColumnIndex(3);
            if (player.effective_healing >= 1'000'000) {
                ImGui::Text("%.2fM", player.effective_healing / 1'000'000.0);
            } else if (player.effective_healing >= 1'000) {
                ImGui::Text("%.1fk", player.effective_healing / 1'000.0);
            } else {
                ImGui::Text("%llu", static_cast<unsigned long long>(player.effective_healing));
            }

            ImGui::TableSetColumnIndex(4);
            ImGui::Text("%.1f%%", player.overheal_pct());
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

    // Pull Selector Combo
    char preview[64];
    if (m_selected_history_pull >= 0 && m_selected_history_pull < static_cast<int>(history.size())) {
        const auto& p = history[m_selected_history_pull];
        uint32_t s = static_cast<uint32_t>(p.duration_seconds);
        std::snprintf(preview, sizeof(preview), "Pull #%d (%02u:%02u - %.0f DPS)",
                      m_selected_history_pull + 1, s / 60, s % 60, p.total_dps);
    } else {
        std::snprintf(preview, sizeof(preview), "Select Pull (Latest: #%zu)", history.size());
    }

    if (ImGui::BeginCombo("##PullCombo", preview)) {
        for (int i = static_cast<int>(history.size()) - 1; i >= 0; --i) {
            const auto& p = history[i];
            uint32_t s = static_cast<uint32_t>(p.duration_seconds);
            char item[64];
            std::snprintf(item, sizeof(item), "Pull #%d (%02u:%02u - %.0f DPS)", i + 1, s / 60, s % 60, p.total_dps);
            bool is_selected = (m_selected_history_pull == i);
            if (ImGui::Selectable(item, is_selected)) {
                m_selected_history_pull = i;
            }
            if (is_selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }

    int idx = (m_selected_history_pull >= 0 && m_selected_history_pull < static_cast<int>(history.size()))
                  ? m_selected_history_pull
                  : static_cast<int>(history.size()) - 1;

    render_damage_tab(history[idx]);
}

void CombatOverlay::render() {
    if (!should_render()) return;

    const float opacity = std::clamp(m_opacity.load(), 0.1f, 1.0f);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing;

    if (m_locked.load()) {
        flags |= ImGuiWindowFlags_NoMove;
    }

    ImGui::SetNextWindowPos(ImVec2(m_pos_x, m_pos_y), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(m_width, m_height), ImGuiCond_FirstUseEver);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.09f, 0.12f, opacity));

    if (ImGui::Begin(overlay_id(), nullptr, flags)) {
        ImVec2 cur_pos = ImGui::GetWindowPos();
        ImVec2 cur_size = ImGui::GetWindowSize();
        m_pos_x = cur_pos.x;
        m_pos_y = cur_pos.y;
        m_width = cur_size.x;
        m_height = cur_size.y;

        EncounterSummary current = m_engine ? m_engine->current_summary() : EncounterSummary{};

        render_top_bar(current);
        ImGui::Separator();

        switch (m_active_tab) {
            case OverlayTab::Damage:
                render_damage_tab(current);
                break;
            case OverlayTab::Healing:
                render_healing_tab(current);
                break;
            case OverlayTab::History:
                render_history_tab();
                break;
        }
    }
    ImGui::End();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

} // namespace hub::meter

#else // !_WIN32 - Cross-platform mock implementation

namespace hub::meter {

CombatOverlay::CombatOverlay(EncounterEngine* engine)
    : m_engine(engine) {
    set_geometry(Rect{50.0f, 100.0f, 420.0f, 220.0f});
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
