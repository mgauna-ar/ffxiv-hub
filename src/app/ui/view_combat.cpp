#include "app/ui/view_combat.hpp"
#include "app/ui/theme.hpp"
#include <algorithm>
#include <vector>

#ifdef _WIN32
#if __has_include("third_party/imgui/imgui.h")
#include "third_party/imgui/imgui.h"
#define HAVE_IMGUI 1
#elif __has_include("imgui.h")
#include "imgui.h"
#define HAVE_IMGUI 1
#endif
#endif

namespace hub::app::ui {

#ifdef HAVE_IMGUI
namespace {

enum class CombatSubTab {
    Damage,
    Healing,
    History,
    OverlaySettings
};

CombatSubTab s_active_subtab = CombatSubTab::Damage;
int s_selected_pull_idx = -1; // -1 = Live encounter
uint32_t s_selected_drilldown_entity = 0;

void render_row_progress_bar(float fraction, uint32_t color_u32) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 p_min = ImGui::GetCursorScreenPos();
    float row_width = ImGui::GetContentRegionAvail().x;
    float row_height = ImGui::GetTextLineHeightWithSpacing();

    ImVec2 p_max = ImVec2(p_min.x + (row_width * std::clamp(fraction, 0.0f, 1.0f)), p_min.y + row_height);
    draw_list->AddRectFilled(p_min, p_max, color_u32);
}

} // namespace
#endif

void render_view_combat(AppState& app_state) {
#ifdef HAVE_IMGUI
    auto live_summary = app_state.get_live_summary();
    auto pull_history = app_state.get_pull_history();

    // Determine current active summary (Live or Pull from history)
    meter::EncounterSummary current_summary = live_summary;
    bool is_live = (s_selected_pull_idx < 0 || static_cast<size_t>(s_selected_pull_idx) >= pull_history.size());
    if (!is_live) {
        current_summary = pull_history[s_selected_pull_idx];
    }

    // Top Bar: Encounter Selector, Duration, DPS/HPS, State Pill, Reset
    ImGui::BeginChild("##CombatTopBar", ImVec2(0.0f * ui_scale(), 62.0f * ui_scale()), true);

    // Pull Selector Combobox
    ImGui::SetNextItemWidth(160.0f * ui_scale());
    std::string current_pull_name = is_live ? "Live Encounter" : ("Pull #" + std::to_string(s_selected_pull_idx + 1));
    if (ImGui::BeginCombo("##PullSelector", current_pull_name.c_str())) {
        if (ImGui::Selectable("Live Encounter", is_live)) {
            s_selected_pull_idx = -1;
        }
        for (size_t i = 0; i < pull_history.size(); ++i) {
            std::string label = "Pull #" + std::to_string(i + 1) + " (" + format_duration(static_cast<uint64_t>(pull_history[i].duration_seconds)) + ")";
            if (ImGui::Selectable(label.c_str(), s_selected_pull_idx == static_cast<int>(i))) {
                s_selected_pull_idx = static_cast<int>(i);
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.55f, 0.59f, 0.67f, 1.0f), "|");
    ImGui::SameLine();

    // Duration
    ImGui::Text("Duration: %s", format_duration(static_cast<uint64_t>(current_summary.duration_seconds)).c_str());

    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.55f, 0.59f, 0.67f, 1.0f), "|");
    ImGui::SameLine();

    // Raid DPS
    ImGui::Text("Raid DPS: %s", format_dps(current_summary.total_dps).c_str());

    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.55f, 0.59f, 0.67f, 1.0f), "|");
    ImGui::SameLine();

    // Raid HPS
    ImGui::Text("Raid HPS: %s", format_dps(current_summary.total_hps).c_str());

    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.55f, 0.59f, 0.67f, 1.0f), "|");
    ImGui::SameLine();

    // Encounter State
    ImVec4 state_col = ImVec4(0.55f, 0.59f, 0.67f, 1.0f);
    const char* state_str = "Idle";
    switch (current_summary.state) {
        case meter::EncounterState::InCombat:
            state_col = ImVec4(0.063f, 0.725f, 0.506f, 1.0f); // Green
            state_str = "In Combat";
            break;
        case meter::EncounterState::Wipe:
            state_col = ImVec4(0.937f, 0.267f, 0.267f, 1.0f); // Red
            state_str = "Wipe";
            break;
        case meter::EncounterState::Complete:
            state_col = ImVec4(0.95f, 0.78f, 0.25f, 1.0f); // Gold
            state_str = "Complete";
            break;
        default:
            break;
    }
    ImGui::TextColored(state_col, "[%s]", state_str);

    // Reset button on right
    ImGui::SameLine(ImGui::GetWindowWidth() - 150.0f * ui_scale());
    if (ImGui::Button("Reset Encounter", ImVec2(135.0f * ui_scale(), 26.0f * ui_scale()))) {
        app_state.reset_encounter();
        s_selected_pull_idx = -1;
        s_selected_drilldown_entity = 0;
    }

    ImGui::EndChild();
    ImGui::Spacing();

    // Subtabs: Damage, Healing, Pull History, Overlay Settings
    if (ImGui::Button("Damage", ImVec2(100.0f * ui_scale(), 30.0f * ui_scale()))) s_active_subtab = CombatSubTab::Damage;
    ImGui::SameLine();
    if (ImGui::Button("Healing", ImVec2(100.0f * ui_scale(), 30.0f * ui_scale()))) s_active_subtab = CombatSubTab::Healing;
    ImGui::SameLine();
    if (ImGui::Button("History", ImVec2(100.0f * ui_scale(), 30.0f * ui_scale()))) s_active_subtab = CombatSubTab::History;
    ImGui::SameLine();
    if (ImGui::Button("Overlay Settings", ImVec2(140.0f * ui_scale(), 30.0f * ui_scale()))) s_active_subtab = CombatSubTab::OverlaySettings;

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // Subtab Content
    if (s_active_subtab == CombatSubTab::Damage) {
        auto combatants = current_summary.combatants;
        std::sort(combatants.begin(), combatants.end(), [](const meter::CombatantStats& a, const meter::CombatantStats& b) {
            return a.dps > b.dps;
        });

        double top_dps = combatants.empty() ? 1.0 : std::max(combatants.front().dps, 1.0);

        if (ImGui::BeginTable("##DamageRankingTable", 9, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 30.0f * ui_scale());
            ImGui::TableSetupColumn("Job", ImGuiTableColumnFlags_WidthFixed, 50.0f * ui_scale());
            ImGui::TableSetupColumn("Combatant", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("DPS", ImGuiTableColumnFlags_WidthFixed, 90.0f * ui_scale());
            ImGui::TableSetupColumn("Damage", ImGuiTableColumnFlags_WidthFixed, 90.0f * ui_scale());
            ImGui::TableSetupColumn("Share", ImGuiTableColumnFlags_WidthFixed, 65.0f * ui_scale());
            ImGui::TableSetupColumn("Crit %", ImGuiTableColumnFlags_WidthFixed, 65.0f * ui_scale());
            ImGui::TableSetupColumn("DH %", ImGuiTableColumnFlags_WidthFixed, 65.0f * ui_scale());
            ImGui::TableSetupColumn("CDH %", ImGuiTableColumnFlags_WidthFixed, 65.0f * ui_scale());
            ImGui::TableHeadersRow();

            int rank = 1;
            for (const auto& c : combatants) {
                if (!c.is_friendly() && c.dps == 0.0) continue;

                ImGui::TableNextRow();

                float frac = static_cast<float>(c.dps / top_dps);
                uint32_t bar_col = get_job_color_u32(c.job, 0.25f);
                render_row_progress_bar(frac, bar_col);

                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%d", rank++);

                ImGui::TableSetColumnIndex(1);
                uint32_t job_col = get_job_color_u32(c.job, 1.0f);
                ImGui::TextColored(ImColor(job_col), "%s", meter::to_string(c.job).data());

                ImGui::TableSetColumnIndex(2);
                std::string sel_label = c.name + "##" + std::to_string(c.entity_id);
                bool selected = (s_selected_drilldown_entity == c.entity_id);
                if (ImGui::Selectable(sel_label.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
                    s_selected_drilldown_entity = (selected ? 0 : c.entity_id);
                }

                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%s", format_dps(c.dps).c_str());

                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%s", format_damage(c.total_damage).c_str());

                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%s", format_percentage(c.damage_share_pct).c_str());

                ImGui::TableSetColumnIndex(6);
                ImGui::Text("%s", format_percentage(c.hits.crit_rate()).c_str());

                ImGui::TableSetColumnIndex(7);
                ImGui::Text("%s", format_percentage(c.hits.dh_rate()).c_str());

                ImGui::TableSetColumnIndex(8);
                ImGui::Text("%s", format_percentage(c.hits.cdh_rate()).c_str());
            }

            ImGui::EndTable();
        }
    } else if (s_active_subtab == CombatSubTab::Healing) {
        auto combatants = current_summary.combatants;
        std::sort(combatants.begin(), combatants.end(), [](const meter::CombatantStats& a, const meter::CombatantStats& b) {
            return a.hps > b.hps;
        });

        double top_hps = combatants.empty() ? 1.0 : std::max(combatants.front().hps, 1.0);

        if (ImGui::BeginTable("##HealingRankingTable", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 30.0f * ui_scale());
            ImGui::TableSetupColumn("Job", ImGuiTableColumnFlags_WidthFixed, 50.0f * ui_scale());
            ImGui::TableSetupColumn("Combatant", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("HPS", ImGuiTableColumnFlags_WidthFixed, 90.0f * ui_scale());
            ImGui::TableSetupColumn("Total Heal", ImGuiTableColumnFlags_WidthFixed, 100.0f * ui_scale());
            ImGui::TableSetupColumn("Effective Heal", ImGuiTableColumnFlags_WidthFixed, 110.0f * ui_scale());
            ImGui::TableSetupColumn("Overheal %", ImGuiTableColumnFlags_WidthFixed, 90.0f * ui_scale());
            ImGui::TableHeadersRow();

            int rank = 1;
            for (const auto& c : combatants) {
                if (!c.is_friendly() && c.hps == 0.0) continue;

                ImGui::TableNextRow();

                float frac = static_cast<float>(c.hps / top_hps);
                uint32_t bar_col = get_role_color_u32(meter::Role::Healer, 0.25f);
                render_row_progress_bar(frac, bar_col);

                ImGui::TableSetColumnIndex(0);
                ImGui::Text("%d", rank++);

                ImGui::TableSetColumnIndex(1);
                uint32_t job_col = get_job_color_u32(c.job, 1.0f);
                ImGui::TextColored(ImColor(job_col), "%s", meter::to_string(c.job).data());

                ImGui::TableSetColumnIndex(2);
                std::string sel_label = c.name + "##H" + std::to_string(c.entity_id);
                bool selected = (s_selected_drilldown_entity == c.entity_id);
                if (ImGui::Selectable(sel_label.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
                    s_selected_drilldown_entity = (selected ? 0 : c.entity_id);
                }

                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%s", format_dps(c.hps).c_str());

                ImGui::TableSetColumnIndex(4);
                ImGui::Text("%s", format_damage(c.total_healing).c_str());

                ImGui::TableSetColumnIndex(5);
                ImGui::Text("%s", format_damage(c.effective_healing).c_str());

                ImGui::TableSetColumnIndex(6);
                ImGui::Text("%s", format_percentage(c.overheal_pct()).c_str());
            }

            ImGui::EndTable();
        }
    } else if (s_active_subtab == CombatSubTab::History) {
        ImGui::TextColored(ImVec4(0.95f, 0.96f, 0.98f, 1.0f), "Pull History Archive (%zu recorded)", pull_history.size());
        ImGui::SameLine(ImGui::GetWindowWidth() - 160.0f * ui_scale());
        if (ImGui::Button("Clear History", ImVec2(140.0f * ui_scale(), 26.0f * ui_scale()))) {
            app_state.clear_pull_history();
            s_selected_pull_idx = -1;
        }

        ImGui::Spacing();

        if (ImGui::BeginTable("##PullHistoryTable", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Pull", ImGuiTableColumnFlags_WidthFixed, 60.0f * ui_scale());
            ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthFixed, 90.0f * ui_scale());
            ImGui::TableSetupColumn("Raid DPS", ImGuiTableColumnFlags_WidthFixed, 100.0f * ui_scale());
            ImGui::TableSetupColumn("Total Damage", ImGuiTableColumnFlags_WidthFixed, 110.0f * ui_scale());
            ImGui::TableSetupColumn("Outcome", ImGuiTableColumnFlags_WidthFixed, 80.0f * ui_scale());
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (size_t i = 0; i < pull_history.size(); ++i) {
                const auto& pull = pull_history[i];
                ImGui::TableNextRow();

                ImGui::TableSetColumnIndex(0);
                ImGui::Text("#%zu", i + 1);

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s", format_duration(static_cast<uint64_t>(pull.duration_seconds)).c_str());

                ImGui::TableSetColumnIndex(2);
                ImGui::Text("%s", format_dps(pull.total_dps).c_str());

                ImGui::TableSetColumnIndex(3);
                ImGui::Text("%s", format_damage(pull.total_damage).c_str());

                ImGui::TableSetColumnIndex(4);
                if (pull.state == meter::EncounterState::Wipe) {
                    ImGui::TextColored(ImVec4(0.937f, 0.267f, 0.267f, 1.0f), "Wipe");
                } else if (pull.state == meter::EncounterState::Complete) {
                    ImGui::TextColored(ImVec4(0.95f, 0.78f, 0.25f, 1.0f), "Clear");
                } else {
                    ImGui::Text("Timeout");
                }

                ImGui::TableSetColumnIndex(5);
                std::string inspect_btn = "Inspect Pull ##" + std::to_string(i);
                if (ImGui::Button(inspect_btn.c_str(), ImVec2(100.0f * ui_scale(), 22.0f * ui_scale()))) {
                    s_selected_pull_idx = static_cast<int>(i);
                    s_active_subtab = CombatSubTab::Damage;
                }
            }

            ImGui::EndTable();
        }
    } else if (s_active_subtab == CombatSubTab::OverlaySettings) {
        ImGui::TextColored(ImVec4(0.95f, 0.96f, 0.98f, 1.0f), "In-Game Combat Meter Overlay Configuration");
        ImGui::TextColored(ImVec4(0.55f, 0.59f, 0.67f, 1.0f), "Adjust display settings synced directly with hub_payload.dll");
        ImGui::Spacing();

        static bool show_overlay = true;
        if (ImGui::Checkbox("Show In-Game Overlay", &show_overlay)) {
            app_state.send_combat_overlay_visible(show_overlay);
        }

        static bool lock_overlay = false;
        if (ImGui::Checkbox("Lock Overlay Position & Size", &lock_overlay)) {
            app_state.send_combat_overlay_locked(lock_overlay);
        }

        static bool click_through = false;
        if (ImGui::Checkbox("Click-Through Mode (Ctrl+\\)", &click_through)) {
            app_state.send_combat_overlay_click_through(click_through);
        }

        static bool auto_hide = false;
        if (ImGui::Checkbox("Auto-Hide When Inactive", &auto_hide)) {
            app_state.send_combat_overlay_auto_hide(auto_hide);
        }

        static bool party_only = true;
        if (ImGui::Checkbox("Filter Party Members Only", &party_only)) {
            app_state.send_combat_overlay_party_only(party_only);
        }

        ImGui::Spacing();

        static float opacity = 0.85f;
        if (ImGui::SliderFloat("Background Opacity", &opacity, 0.20f, 1.00f, "%.2f")) {
            app_state.send_combat_overlay_opacity(opacity);
        }

        static float scale = 1.0f;
        if (ImGui::SliderFloat("UI Scale", &scale, 0.70f, 1.50f, "%.2fx")) {
            app_state.send_combat_overlay_scale(scale);
        }
    }

    // Selected Player Ability Breakdown Drilldown Panel
    if (s_selected_drilldown_entity != 0) {
        const meter::CombatantStats* selected_c = nullptr;
        for (const auto& c : current_summary.combatants) {
            if (c.entity_id == s_selected_drilldown_entity) {
                selected_c = &c;
                break;
            }
        }

        if (selected_c) {
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            ImGui::TextColored(ImVec4(0.231f, 0.510f, 0.965f, 1.0f), "Ability Breakdown: %s (%s)",
                selected_c->name.c_str(), meter::to_string(selected_c->job).data());
            ImGui::SameLine(ImGui::GetWindowWidth() - 140.0f * ui_scale());
            if (ImGui::Button("Close Breakdown", ImVec2(120.0f * ui_scale(), 24.0f * ui_scale()))) {
                s_selected_drilldown_entity = 0;
            }

            ImGui::Spacing();

            std::vector<meter::ActionSummary> actions;
            for (const auto& [_, act] : selected_c->actions) {
                actions.push_back(act);
            }
            std::sort(actions.begin(), actions.end(), [](const meter::ActionSummary& a, const meter::ActionSummary& b) {
                return a.total_damage > b.total_damage;
            });

            if (ImGui::BeginTable("##DrilldownTable", 7, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Casts", ImGuiTableColumnFlags_WidthFixed, 60.0f * ui_scale());
                ImGui::TableSetupColumn("Total Dmg", ImGuiTableColumnFlags_WidthFixed, 90.0f * ui_scale());
                ImGui::TableSetupColumn("Min", ImGuiTableColumnFlags_WidthFixed, 75.0f * ui_scale());
                ImGui::TableSetupColumn("Avg", ImGuiTableColumnFlags_WidthFixed, 75.0f * ui_scale());
                ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthFixed, 75.0f * ui_scale());
                ImGui::TableSetupColumn("Crit %", ImGuiTableColumnFlags_WidthFixed, 65.0f * ui_scale());
                ImGui::TableHeadersRow();

                for (const auto& act : actions) {
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%s", act.name.c_str());

                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%llu", static_cast<unsigned long long>(act.hit_count));

                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%s", format_damage(act.total_damage).c_str());

                    ImGui::TableSetColumnIndex(3);
                    ImGui::Text("%s", format_damage(act.min_damage).c_str());

                    ImGui::TableSetColumnIndex(4);
                    ImGui::Text("%s", format_damage(static_cast<uint64_t>(act.average_damage())).c_str());

                    ImGui::TableSetColumnIndex(5);
                    ImGui::Text("%s", format_damage(act.max_damage).c_str());

                    ImGui::TableSetColumnIndex(6);
                    ImGui::Text("%s", format_percentage(act.hits.crit_rate()).c_str());
                }

                ImGui::EndTable();
            }
        }
    }
#else
    (void)app_state;
#endif
}

} // namespace hub::app::ui
