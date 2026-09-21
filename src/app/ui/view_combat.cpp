#include "app/ui/view_combat.hpp"
#include "app/ui/config_binding.hpp"
#include "app/ui/icons.hpp"
#include "app/ui/overlay_settings.hpp"
#include "app/ui/theme.hpp"
#include "app/ui/widgets.hpp"
#include "meter/pull_grouping.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace hub::app::ui {

#ifdef HAVE_IMGUI
namespace {

constexpr const char* METER = "combat_meter";

/// Keyed on encounter_id, not on a position in the archive: the history deque
/// evicts from the front at capacity, so an index stops meaning the same pull.
/// 0 = live encounter.
uint64_t s_selected_pull_id = 0;
uint32_t s_selected_drilldown_entity = 0;
/// Set when a History row asks to inspect a pull, so the tab bar can switch away
/// from History on the next frame.
bool s_jump_to_damage = false;

/// A full EncounterSummary carries every combatant's per-action breakdown, so
/// refetching one per frame means hundreds of map and string allocations under
/// the combat mutex. Snapshot on a timer instead, like the in-game overlay does.
constexpr long long kSnapshotIntervalMs = 250;
meter::EncounterSummary s_live_summary;
meter::EncounterSummary s_selected_pull;
uint64_t s_cached_pull_id = 0;
std::chrono::steady_clock::time_point s_last_snapshot{};

constexpr ImGuiTableFlags kTableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                        ImGuiTableFlags_BordersInnerV |
                                        ImGuiTableFlags_SizingStretchProp;

void table_headers_row() {
    ImGui::PushStyleColor(ImGuiCol_Text, v4(colors::TextDim));
    ImGui::TableHeadersRow();
    ImGui::PopStyleColor();
}

/// Position of a pull in the archive listing, or nothing if it has been evicted.
std::optional<size_t> find_pull_index(const std::vector<meter::PullHistoryEntry>& pull_history,
                                      uint64_t encounter_id) {
    if (encounter_id == 0) return std::nullopt;
    for (size_t i = 0; i < pull_history.size(); ++i) {
        if (pull_history[i].encounter_id == encounter_id) return i;
    }
    return std::nullopt;
}

/// Local clock time a pull ended. Pulls archived before this was recorded (and
/// the live encounter) carry 0.
std::string format_clock_time(uint64_t unix_seconds) {
    if (unix_seconds == 0) return "--:--";
    const auto t = static_cast<std::time_t>(unix_seconds);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
    return buf;
}

/// Encounter state as a badge, so the live/wipe/clear distinction is visible at a
/// glance instead of being one more grey word in a row of separators.
void encounter_state_pill(meter::EncounterState state) {
    switch (state) {
        case meter::EncounterState::InCombat: pill("In combat", colors::SuccessLight); return;
        case meter::EncounterState::Wipe:     pill("Wipe", colors::Danger); return;
        case meter::EncounterState::Complete: pill("Clear", colors::WarningLight); return;
        default: break;
    }
    pill("Idle", colors::TextDim);
}

void render_top_bar(AppState& app_state, const meter::EncounterSummary& summary,
                    const std::vector<meter::PullHistoryEntry>& pull_history, bool is_live) {
    begin_card("##CombatTopBar", ImVec2(0.0f, m(62.0f)));

    const float row_y = ImGui::GetCursorPosY();
    const auto selected_idx = find_pull_index(pull_history, s_selected_pull_id);

    std::string current_pull_name = "Live encounter";
    if (!is_live && selected_idx) {
        const auto& sel = pull_history[*selected_idx];
        std::string zone = meter::zone_label(sel.zone_id, sel.zone_name);
        if (zone.empty()) zone = "Unknown zone";
        current_pull_name = zone + "  -  Pull #" + std::to_string(sel.encounter_id);
    }

    // The popup carries duty names, which do not fit the closed control.
    ImGui::SetNextWindowSizeConstraints(ImVec2(m(300.0f), 0.0f), ImVec2(m(600.0f), m(420.0f)));
    ImGui::SetNextItemWidth(m(240.0f));
    if (ImGui::BeginCombo("##PullSelector", current_pull_name.c_str())) {
        if (ImGui::Selectable("Live encounter", is_live)) {
            s_selected_pull_id = 0;
        }

        const auto groups = meter::group_pulls_by_zone(pull_history);
        for (size_t g = 0; g < groups.size(); ++g) {
            const auto& group = groups[g];
            const bool holds_selection =
                selected_idx && std::find(group.pulls.begin(), group.pulls.end(), *selected_idx)
                                    != group.pulls.end();

            // Opened once so the newest duty (and whichever holds the current
            // selection) starts expanded; after that the user's own toggling wins.
            ImGui::PushID(static_cast<int>(group.zone_id));
            ImGui::SetNextItemOpen(g == 0 || holds_selection, ImGuiCond_Once);
            const std::string header = group.label + "  (" + std::to_string(group.pulls.size()) + ")";
            if (ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
                for (size_t idx : group.pulls) {
                    const auto& pull = pull_history[idx];
                    const std::string label =
                        "Pull #" + std::to_string(pull.encounter_id) + "  (" +
                        format_duration(static_cast<uint64_t>(pull.duration_seconds)) + ")";
                    if (ImGui::Selectable(label.c_str(), s_selected_pull_id == pull.encounter_id)) {
                        s_selected_pull_id = pull.encounter_id;
                    }
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }

    // Label-over-value pairs rather than a pipe-separated run of text: the numbers
    // are the point of this bar and need to read first.
    const auto stat = [&](const char* label, const std::string& value, uint32_t color) {
        ImGui::SameLine(0.0f, m(20.0f));
        ImGui::BeginGroup();
        ImGui::SetCursorPosY(row_y - m(3.0f));
        text_colored_u32(colors::TextDim, "%s", label);
        ImGui::PushFont(bold_font());
        text_colored_u32(color, "%s", value.c_str());
        ImGui::PopFont();
        ImGui::EndGroup();
    };

    stat("DURATION", format_duration(static_cast<uint64_t>(summary.duration_seconds)), colors::TextPrimary);
    stat("RAID DPS", format_dps(summary.total_dps), colors::AccentHover);
    stat("RAID HPS", format_dps(summary.total_hps), colors::SuccessLight);
    stat("COMBATANTS", std::to_string(summary.combatants.size()), colors::TextPrimary);

    ImGui::SameLine();
    right_align(m(metrics::ButtonMd) + m(120.0f));
    ImGui::SetCursorPosY(row_y + m(4.0f));
    encounter_state_pill(summary.state);

    ImGui::SameLine();
    right_align(m(metrics::ButtonMd));
    ImGui::SetCursorPosY(row_y);
    if (button(ICON_RESET "  Reset encounter", ButtonKind::Danger, ButtonSize::Medium)) {
        app_state.reset_encounter();
        s_selected_pull_id = 0;
        s_selected_drilldown_entity = 0;
    }

    end_card();
}

/// Name cell shared by the damage and healing tables: a job badge, then a
/// row-spanning selectable that drives the drilldown panel.
void combatant_name_cell(const meter::CombatantStats& c, const char* id_prefix) {
    job_badge(c.job);
    ImGui::SameLine(0.0f, m(8.0f));

    const std::string sel_label = c.name + "##" + id_prefix + std::to_string(c.entity_id);
    const bool selected = (s_selected_drilldown_entity == c.entity_id);
    if (ImGui::Selectable(sel_label.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
        s_selected_drilldown_entity = (selected ? 0 : c.entity_id);
    }
}

void render_damage_table(const meter::EncounterSummary& summary, float height) {
    // Sort pointers: the structs carry a per-action map that is not worth copying.
    std::vector<const meter::CombatantStats*> combatants;
    combatants.reserve(summary.combatants.size());
    for (const auto& c : summary.combatants) combatants.push_back(&c);
    std::sort(combatants.begin(), combatants.end(),
              [](const meter::CombatantStats* a, const meter::CombatantStats* b) {
                  return a->dps > b->dps;
              });

    if (combatants.empty()) {
        empty_state(ICON_SWORDS, "No damage recorded",
                    "Pull something and the ranking fills in live.");
        return;
    }

    const double top_dps = std::max(combatants.front()->dps, 1.0);
    if (!ImGui::BeginTable("##DamageRankingTable", 8, kTableFlags, ImVec2(0.0f, height))) return;

    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, m(30.0f));
    ImGui::TableSetupColumn("Combatant", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("DPS", ImGuiTableColumnFlags_WidthFixed, m(90.0f));
    ImGui::TableSetupColumn("Damage", ImGuiTableColumnFlags_WidthFixed, m(90.0f));
    ImGui::TableSetupColumn("Share", ImGuiTableColumnFlags_WidthFixed, m(65.0f));
    ImGui::TableSetupColumn("Crit %", ImGuiTableColumnFlags_WidthFixed, m(65.0f));
    ImGui::TableSetupColumn("DH %", ImGuiTableColumnFlags_WidthFixed, m(65.0f));
    ImGui::TableSetupColumn("CDH %", ImGuiTableColumnFlags_WidthFixed, m(65.0f));
    ImGui::TableSetupScrollFreeze(0, 1);
    table_headers_row();

    int rank = 1;
    for (const meter::CombatantStats* cp : combatants) {
        const auto& c = *cp;
        if (!c.is_friendly() && c.dps == 0.0) continue;

        ImGui::TableNextRow();
        row_progress_bar(static_cast<float>(c.dps / top_dps), get_job_color_u32(c.job));

        ImGui::TableSetColumnIndex(0);
        text_colored_u32(colors::TextDim, "%d", rank++);

        ImGui::TableSetColumnIndex(1);
        combatant_name_cell(c, "D");

        ImGui::TableSetColumnIndex(2);
        ImGui::PushFont(bold_font());
        text_colored_u32(get_job_color_u32(c.job), "%s", format_dps(c.dps).c_str());
        ImGui::PopFont();

        ImGui::TableSetColumnIndex(3);
        text_colored_u32(colors::TextBody, "%s", format_damage(c.total_damage).c_str());

        ImGui::TableSetColumnIndex(4);
        text_colored_u32(colors::TextMuted, "%s", format_percentage(c.damage_share_pct).c_str());

        ImGui::TableSetColumnIndex(5);
        text_colored_u32(colors::TextMuted, "%s", format_percentage(c.hits.crit_rate()).c_str());

        ImGui::TableSetColumnIndex(6);
        text_colored_u32(colors::TextMuted, "%s", format_percentage(c.hits.dh_rate()).c_str());

        ImGui::TableSetColumnIndex(7);
        text_colored_u32(colors::TextMuted, "%s", format_percentage(c.hits.cdh_rate()).c_str());
    }

    ImGui::EndTable();
}

void render_healing_table(const meter::EncounterSummary& summary, float height) {
    std::vector<const meter::CombatantStats*> combatants;
    combatants.reserve(summary.combatants.size());
    for (const auto& c : summary.combatants) combatants.push_back(&c);
    std::sort(combatants.begin(), combatants.end(),
              [](const meter::CombatantStats* a, const meter::CombatantStats* b) {
                  return a->hps > b->hps;
              });

    if (combatants.empty()) {
        empty_state(ICON_HEART, "No healing recorded",
                    "Healing output appears here as soon as a pull starts.");
        return;
    }

    const double top_hps = std::max(combatants.front()->hps, 1.0);
    if (!ImGui::BeginTable("##HealingRankingTable", 6, kTableFlags, ImVec2(0.0f, height))) return;

    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, m(30.0f));
    ImGui::TableSetupColumn("Combatant", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("HPS", ImGuiTableColumnFlags_WidthFixed, m(90.0f));
    ImGui::TableSetupColumn("Total heal", ImGuiTableColumnFlags_WidthFixed, m(100.0f));
    ImGui::TableSetupColumn("Effective", ImGuiTableColumnFlags_WidthFixed, m(110.0f));
    ImGui::TableSetupColumn("Overheal", ImGuiTableColumnFlags_WidthFixed, m(90.0f));
    ImGui::TableSetupScrollFreeze(0, 1);
    table_headers_row();

    int rank = 1;
    for (const meter::CombatantStats* cp : combatants) {
        const auto& c = *cp;
        if (!c.is_friendly() && c.hps == 0.0) continue;

        ImGui::TableNextRow();
        row_progress_bar(static_cast<float>(c.hps / top_hps), colors::Success);

        ImGui::TableSetColumnIndex(0);
        text_colored_u32(colors::TextDim, "%d", rank++);

        ImGui::TableSetColumnIndex(1);
        combatant_name_cell(c, "H");

        ImGui::TableSetColumnIndex(2);
        ImGui::PushFont(bold_font());
        text_colored_u32(colors::SuccessLight, "%s", format_dps(c.hps).c_str());
        ImGui::PopFont();

        ImGui::TableSetColumnIndex(3);
        text_colored_u32(colors::TextBody, "%s", format_damage(c.total_healing).c_str());

        ImGui::TableSetColumnIndex(4);
        text_colored_u32(colors::TextBody, "%s", format_damage(c.effective_healing).c_str());

        ImGui::TableSetColumnIndex(5);
        text_colored_u32(colors::TextMuted, "%s", format_percentage(c.overheal_pct()).c_str());
    }

    ImGui::EndTable();
}

void render_history_table(AppState& app_state,
                          const std::vector<meter::PullHistoryEntry>& pull_history) {
    right_align(m(metrics::ButtonMd));
    if (button(ICON_TRASH "  Clear history", ButtonKind::Danger, ButtonSize::Medium)) {
        app_state.clear_pull_history();
        s_selected_pull_id = 0;
    }

    if (pull_history.empty()) {
        empty_state(ICON_HISTORY, "No pulls archived yet",
                    "Finished encounters are kept here for comparison.");
        return;
    }

    if (!ImGui::BeginTable("##PullHistoryTable", 9, kTableFlags, ImVec2(0.0f, fill_h(0.0f)))) return;

    ImGui::TableSetupColumn("Pull", ImGuiTableColumnFlags_WidthFixed, m(60.0f));
    ImGui::TableSetupColumn("Ended", ImGuiTableColumnFlags_WidthFixed, m(70.0f));
    ImGui::TableSetupColumn("Zone", ImGuiTableColumnFlags_WidthFixed, m(90.0f));
    ImGui::TableSetupColumn("Duration", ImGuiTableColumnFlags_WidthFixed, m(90.0f));
    ImGui::TableSetupColumn("Raid DPS", ImGuiTableColumnFlags_WidthFixed, m(100.0f));
    ImGui::TableSetupColumn("Raid HPS", ImGuiTableColumnFlags_WidthFixed, m(100.0f));
    ImGui::TableSetupColumn("Total damage", ImGuiTableColumnFlags_WidthFixed, m(110.0f));
    ImGui::TableSetupColumn("Outcome", ImGuiTableColumnFlags_WidthFixed, m(100.0f));
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupScrollFreeze(0, 1);
    table_headers_row();

    for (size_t i = 0; i < pull_history.size(); ++i) {
        const auto& pull = pull_history[i];
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        text_colored_u32(colors::TextMuted, "#%llu",
                         static_cast<unsigned long long>(pull.encounter_id));

        ImGui::TableSetColumnIndex(1);
        text_colored_u32(colors::TextMuted, "%s", format_clock_time(pull.ended_at_unix_s).c_str());

        ImGui::TableSetColumnIndex(2);
        const std::string zone = meter::zone_label(pull.zone_id, pull.zone_name);
        text_colored_u32(colors::TextMuted, "%s", zone.empty() ? "--" : zone.c_str());

        ImGui::TableSetColumnIndex(3);
        text_colored_u32(colors::TextBody, "%s",
                         format_duration(static_cast<uint64_t>(pull.duration_seconds)).c_str());

        ImGui::TableSetColumnIndex(4);
        text_colored_u32(colors::AccentHover, "%s", format_dps(pull.total_dps).c_str());

        ImGui::TableSetColumnIndex(5);
        text_colored_u32(colors::SuccessLight, "%s", format_dps(pull.total_hps).c_str());

        ImGui::TableSetColumnIndex(6);
        text_colored_u32(colors::TextBody, "%s", format_damage(pull.total_damage).c_str());

        ImGui::TableSetColumnIndex(7);
        if (pull.state == meter::EncounterState::Wipe) {
            pill("Wipe", colors::Danger);
        } else if (pull.state == meter::EncounterState::Complete) {
            pill("Clear", colors::WarningLight);
        } else {
            pill("Timeout", colors::TextDim);
        }

        ImGui::TableSetColumnIndex(8);
        const std::string inspect_btn = std::string(ICON_SEARCH "  Inspect##") + std::to_string(i);
        if (button(inspect_btn.c_str(), ButtonKind::Secondary, ButtonSize::Small)) {
            s_selected_pull_id = pull.encounter_id;
            s_jump_to_damage = true;
        }
    }

    ImGui::EndTable();
}

void render_overlay_card(AppState& app_state) {
    CardOptions opts{};
    opts.auto_height = true;
    begin_card("##MeterOverlayCard", ImVec2(0.0f, 0.0f), opts);

    OverlaySettingsOptions overlay_opts{};
    overlay_opts.section = METER;
    overlay_opts.plugin = PluginId::CombatMeter;
    overlay_opts.visible_label = "Show in-game meter";
    overlay_opts.locked_label = "Lock meter position & size";
    overlay_opts.opacity_label = "Background opacity";
    overlay_opts.scale_label = "Meter scale";
    overlay_opts.min_opacity = meter::constants::MIN_WINDOW_OPACITY;
    overlay_opts.max_opacity = meter::constants::MAX_WINDOW_OPACITY;
    overlay_opts.min_scale = meter::constants::MIN_UI_SCALE;
    overlay_opts.max_scale = meter::constants::MAX_UI_SCALE;
    overlay_opts.defaults = meter::CombatConfig{}.overlay;
    render_overlay_settings(app_state, overlay_opts);

    static const char* metric_modes[] = { "Damage", "Healing" };
    int metric = cfg_int(METER, "overlay_metric", 0);
    begin_setting_row("Meter metric", "Which table the in-game meter draws.");
    if (ImGui::Combo("##meter_metric", &metric, metric_modes, 2)) {
        cfg_store(METER, "overlay_metric", metric);
        app_state.send_combat_overlay_metric(static_cast<uint32_t>(metric));
    }
    end_setting_row();

    end_card();
}

void render_columns_card(AppState& app_state) {
    CardOptions opts{};
    opts.auto_height = true;
    begin_card("##MeterColumnsCard", ImVec2(0.0f, 0.0f), opts);
    section_header(ICON_CHECKLIST, "TABLE COLUMNS", colors::Violet);

    bool col_share = cfg_bool(METER, "show_col_share", true);
    if (setting_toggle("Damage share", "Percentage of total raid damage.", &col_share)) {
        cfg_store(METER, "show_col_share", col_share);
        app_state.send_combat_column_share(col_share);
    }

    bool col_crit = cfg_bool(METER, "show_col_crit", true);
    if (setting_toggle("Critical hit rate", "Per-combatant crit percentage.", &col_crit)) {
        cfg_store(METER, "show_col_crit", col_crit);
        app_state.send_combat_column_crit(col_crit);
    }

    bool col_dh = cfg_bool(METER, "show_col_dh", true);
    if (setting_toggle("Direct hit rate", "Per-combatant direct hit percentage.", &col_dh)) {
        cfg_store(METER, "show_col_dh", col_dh);
        app_state.send_combat_column_dh(col_dh);
    }

    bool col_cdh = cfg_bool(METER, "show_col_cdh", true);
    if (setting_toggle("Critical direct hit rate", "Combined crit-and-direct percentage.", &col_cdh)) {
        cfg_store(METER, "show_col_cdh", col_cdh);
        app_state.send_combat_column_cdh(col_cdh);
    }

    end_card();
}

void render_behaviour_card(AppState& app_state) {
    CardOptions opts{};
    opts.auto_height = true;
    begin_card("##MeterBehaviourCard", ImVec2(0.0f, 0.0f), opts);
    section_header(ICON_SLIDERS, "METER BEHAVIOUR", colors::Warning);

    bool party_only = cfg_bool(METER, "party_only", true);
    if (setting_toggle("Party members only", "Exclude everyone outside your party.", &party_only)) {
        cfg_store(METER, "party_only", party_only);
        app_state.send_combat_overlay_party_only(party_only);
    }

    bool show_bars = cfg_bool(METER, "show_bars", true);
    if (setting_toggle("Job-coloured row bars", "Draws a role-tinted bar behind each row.", &show_bars)) {
        cfg_store(METER, "show_bars", show_bars);
        app_state.send_combat_show_bars(show_bars);
    }

    bool hide_inactive = cfg_bool(METER, "hide_inactive", false);
    if (setting_toggle("Hide idle combatants", "Drops anyone with no contribution this pull.", &hide_inactive)) {
        cfg_store(METER, "hide_inactive", hide_inactive);
        app_state.send_combat_hide_inactive(hide_inactive);
    }

    int refresh_ms = cfg_int(METER, "refresh_interval_ms", 500);
    begin_setting_row("Overlay refresh", "How often the in-game meter redraws.");
    if (ImGui::SliderInt("##refresh_ms", &refresh_ms, 100, 2000, "%d ms")) {
        cfg_store(METER, "refresh_interval_ms", refresh_ms);
        app_state.send_combat_refresh_interval(static_cast<uint32_t>(refresh_ms));
    }
    end_setting_row();

    float timeout_s = cfg_float(METER, "inactivity_timeout_seconds", 7.0f);
    begin_setting_row("End encounter after idle", "Inactivity before a pull is closed out.");
    if (ImGui::SliderFloat("##idle_timeout", &timeout_s, 3.0f, 60.0f, "%.0f s")) {
        cfg_store(METER, "inactivity_timeout_seconds", timeout_s);
        app_state.send_combat_inactivity_timeout(timeout_s);
    }
    end_setting_row();

    ImGui::Dummy(ImVec2(0.0f, m(4.0f)));
    if (button(ICON_MOVE "  Reset overlay position", ButtonKind::Secondary, ButtonSize::Large)) {
        app_state.send_combat_reset_overlay_geometry();
    }
    ImGui::SameLine(0.0f, m(8.0f));
    if (button(ICON_POWER "  End encounter", ButtonKind::Secondary, ButtonSize::Medium)) {
        app_state.send_combat_end_encounter();
    }
    ImGui::Dummy(ImVec2(0.0f, m(4.0f)));
    if (button(ICON_TRASH "  Reset all statistics", ButtonKind::Danger, ButtonSize::Large)) {
        app_state.send_combat_reset_stats();
        app_state.reset_encounter();
        app_state.clear_pull_history();
    }

    end_card();
}

void render_settings_tab(AppState& app_state) {
    const int columns = settings_columns(2);
    const float col_w = split_w(columns);
    const float col_h = fill_h(0.0f);

    ImGui::BeginChild("##MeterSettingsLeft", ImVec2(columns > 1 ? col_w : 0.0f, col_h),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    render_overlay_card(app_state);
    if (columns == 1) {
        ImGui::Dummy(ImVec2(0.0f, m(metrics::Gutter)));
        render_columns_card(app_state);
        ImGui::Dummy(ImVec2(0.0f, m(metrics::Gutter)));
        render_behaviour_card(app_state);
    }
    ImGui::EndChild();

    if (columns > 1) {
        ImGui::SameLine(0.0f, m(metrics::Gutter));
        ImGui::BeginChild("##MeterSettingsRight", ImVec2(col_w, col_h),
                          ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
        render_columns_card(app_state);
        ImGui::Dummy(ImVec2(0.0f, m(metrics::Gutter)));
        render_behaviour_card(app_state);
        ImGui::EndChild();
    }
}

void render_drilldown(const meter::EncounterSummary& summary) {
    const meter::CombatantStats* selected = nullptr;
    for (const auto& c : summary.combatants) {
        if (c.entity_id == s_selected_drilldown_entity) {
            selected = &c;
            break;
        }
    }
    if (selected == nullptr) return;

    begin_card("##DrilldownCard", ImVec2(0.0f, fill_h(0.0f)));

    char header[128];
    std::snprintf(header, sizeof(header), "ABILITY BREAKDOWN - %s", selected->name.c_str());
    begin_section_header(ICON_CROSSHAIR, header, m(metrics::ButtonSm),
                         get_job_color_u32(selected->job));
    if (button(ICON_CIRCLE_X "  Close", ButtonKind::Secondary, ButtonSize::Small)) {
        s_selected_drilldown_entity = 0;
    }
    end_section_header();

    std::vector<meter::ActionSummary> actions;
    actions.reserve(selected->actions.size());
    for (const auto& [_, act] : selected->actions) {
        actions.push_back(act);
    }
    std::sort(actions.begin(), actions.end(),
              [](const meter::ActionSummary& a, const meter::ActionSummary& b) {
                  return a.total_damage > b.total_damage;
              });

    if (actions.empty()) {
        empty_state(ICON_CROSSHAIR, "No abilities recorded",
                    "This combatant has not landed anything yet.");
        end_card();
        return;
    }

    if (ImGui::BeginTable("##DrilldownTable", 7, kTableFlags, ImVec2(0.0f, fill_h(0.0f)))) {
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Casts", ImGuiTableColumnFlags_WidthFixed, m(60.0f));
        ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_WidthFixed, m(90.0f));
        ImGui::TableSetupColumn("Min", ImGuiTableColumnFlags_WidthFixed, m(75.0f));
        ImGui::TableSetupColumn("Avg", ImGuiTableColumnFlags_WidthFixed, m(75.0f));
        ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthFixed, m(75.0f));
        ImGui::TableSetupColumn("Crit %", ImGuiTableColumnFlags_WidthFixed, m(65.0f));
        ImGui::TableSetupScrollFreeze(0, 1);
        table_headers_row();

        const uint64_t top_damage = std::max<uint64_t>(actions.front().total_damage, 1);
        for (const auto& act : actions) {
            ImGui::TableNextRow();
            row_progress_bar(static_cast<float>(static_cast<double>(act.total_damage) /
                                                static_cast<double>(top_damage)),
                             get_job_color_u32(selected->job));

            ImGui::TableSetColumnIndex(0);
            text_colored_u32(colors::TextBody, "%s", act.name.c_str());

            ImGui::TableSetColumnIndex(1);
            text_colored_u32(colors::TextMuted, "%llu", static_cast<unsigned long long>(act.hit_count));

            ImGui::TableSetColumnIndex(2);
            text_colored_u32(colors::TextPrimary, "%s", format_damage(act.total_damage).c_str());

            ImGui::TableSetColumnIndex(3);
            text_colored_u32(colors::TextMuted, "%s", format_damage(act.min_damage).c_str());

            ImGui::TableSetColumnIndex(4);
            text_colored_u32(colors::TextBody, "%s",
                             format_damage(static_cast<uint64_t>(act.average_damage())).c_str());

            ImGui::TableSetColumnIndex(5);
            text_colored_u32(colors::TextMuted, "%s", format_damage(act.max_damage).c_str());

            ImGui::TableSetColumnIndex(6);
            // DoT ticks carry no severity, so a status row has no crit rate to show.
            if (act.hits.rated_hits() == 0) {
                text_colored_u32(colors::TextMuted, "%s", "-");
            } else {
                text_colored_u32(colors::TextMuted, "%s", format_percentage(act.hits.crit_rate()).c_str());
            }
        }

        ImGui::EndTable();
    }

    end_card();
}

/// The ranking tabs split their height with the drilldown panel when one is open,
/// so opening a breakdown never pushes the table off the bottom of the window.
float ranking_table_height() {
    return s_selected_drilldown_entity != 0 ? fill_h(0.0f) * 0.55f : fill_h(0.0f);
}

} // namespace
#endif

void render_view_combat(AppState& app_state) {
#ifdef HAVE_IMGUI
    const auto pull_history = app_state.get_pull_history_index();

    // A selected pull that has aged out of the archive falls back to live rather
    // than leaving the tables pointing at whatever took its place.
    const auto selected_index = find_pull_index(pull_history, s_selected_pull_id);
    const bool is_live = !selected_index.has_value();

    const auto now = std::chrono::steady_clock::now();
    const bool stale = std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - s_last_snapshot).count() >= kSnapshotIntervalMs;
    if (is_live) {
        if (stale) {
            s_live_summary = app_state.get_live_summary();
            s_last_snapshot = now;
        }
    } else if (stale || s_cached_pull_id != s_selected_pull_id) {
        if (auto pull = app_state.get_pull(*selected_index)) {
            s_selected_pull = std::move(*pull);
        }
        s_cached_pull_id = s_selected_pull_id;
        s_last_snapshot = now;
    }

    const meter::EncounterSummary& current_summary = is_live ? s_live_summary : s_selected_pull;

    page_header(ICON_SWORDS, "Combat Meter",
                "Per-pull damage, healing and ability breakdowns");
    ImGui::Dummy(ImVec2(0.0f, m(4.0f)));

    render_top_bar(app_state, current_summary, pull_history, is_live);
    ImGui::Dummy(ImVec2(0.0f, m(2.0f)));

    if (ImGui::BeginTabBar("##CombatTabs", ImGuiTabBarFlags_None)) {
        ImGuiTabItemFlags damage_flags = ImGuiTabItemFlags_None;
        if (s_jump_to_damage) {
            damage_flags = ImGuiTabItemFlags_SetSelected;
            s_jump_to_damage = false;
        }

        if (ImGui::BeginTabItem(ICON_SWORDS "  Damage", nullptr, damage_flags)) {
            ImGui::Dummy(ImVec2(0.0f, m(4.0f)));
            render_damage_table(current_summary, ranking_table_height());
            render_drilldown(current_summary);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ICON_HEART "  Healing")) {
            ImGui::Dummy(ImVec2(0.0f, m(4.0f)));
            render_healing_table(current_summary, ranking_table_height());
            render_drilldown(current_summary);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ICON_HISTORY "  Pull history")) {
            ImGui::Dummy(ImVec2(0.0f, m(4.0f)));
            render_history_table(app_state, pull_history);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ICON_SLIDERS "  Overlay settings")) {
            ImGui::Dummy(ImVec2(0.0f, m(4.0f)));
            render_settings_tab(app_state);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
#else
    (void)app_state;
#endif
}

} // namespace hub::app::ui
