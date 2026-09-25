#include "app/ui/view_combat.hpp"
#include "app/ui/combat_damage_taken.hpp"
#include "app/ui/combat_deaths.hpp"
#include "app/ui/combat_statuses.hpp"
#include "app/ui/combat_view_common.hpp"
#include "app/ui/config_binding.hpp"
#include "common/ui/icons.hpp"
#include "common/ui/job_style.hpp"
#include "app/ui/overlay_settings.hpp"
#include "app/ui/plugin_page.hpp"
#include "app/ui/theme.hpp"
#include "app/ui/widgets.hpp"
#include "meter/pull_grouping.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <iterator>
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

/// A full EncounterSummary carries every combatant's per-action breakdown, so
/// refetching one per frame means hundreds of map and string allocations under
/// the combat mutex. Snapshot on a timer instead, like the in-game overlay does.
constexpr long long kSnapshotIntervalMs = 250;
meter::EncounterSummary s_live_summary;
meter::EncounterSummary s_selected_pull;
uint64_t s_cached_pull_id = 0;
std::chrono::steady_clock::time_point s_last_snapshot{};

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

/// A status word and the colour it is drawn in.
struct Badge {
    const char* label;
    uint32_t color;
};

/// Encounter state as a badge, so the live/wipe/clear distinction is visible at a
/// glance instead of being one more grey word in a row of separators.
Badge encounter_state_badge(meter::EncounterState state) {
    switch (state) {
        case meter::EncounterState::InCombat: return {"In combat", colors::SuccessLight};
        case meter::EncounterState::Wipe:     return {"Wipe", colors::Danger};
        case meter::EncounterState::Complete: return {"Clear", colors::WarningLight};
        default:                              return {"Idle", colors::TextDim};
    }
}

/// How an archived pull ended. Anything but a wipe or a clear timed out.
Badge pull_outcome_badge(meter::EncounterState state) {
    switch (state) {
        case meter::EncounterState::Wipe:     return {"Wipe", colors::Danger};
        case meter::EncounterState::Complete: return {"Clear", colors::WarningLight};
        default:                              return {"Timeout", colors::TextDim};
    }
}

float rail_row_height() {
    return ImGui::GetTextLineHeight() + m(6.0f);
}

/// One selectable rail row with a badge flush right. The badge is drawn over the
/// selectable, so the whole row stays clickable.
bool rail_row(const char* label, bool selected, Badge badge) {
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.0f, 0.5f));
    const bool clicked = ImGui::Selectable(label, selected, ImGuiSelectableFlags_AllowOverlap,
                                           ImVec2(0.0f, rail_row_height()));
    ImGui::PopStyleVar();
    ImGui::SameLine();
    right_align(pill_width(badge.label));
    pill(badge.label, badge.color);
    return clicked;
}

/// One archived pull: outcome dot, number and duration, then deaths and end time
/// only while they fit, so a narrow rail drops detail instead of clipping it. The
/// tooltip always carries all of it.
bool pull_row(const meter::PullHistoryEntry& pull, bool selected, float number_w) {
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float right = pos.x + ImGui::GetContentRegionAvail().x;
    const float row_h = rail_row_height();
    const std::string id = "##Pull" + std::to_string(pull.encounter_id);
    const bool clicked = ImGui::Selectable(id.c_str(), selected, ImGuiSelectableFlags_None,
                                           ImVec2(0.0f, row_h));

    const Badge outcome = pull_outcome_badge(pull.state);
    const std::string number = "#" + std::to_string(pull.encounter_id);
    const std::string duration = format_duration(static_cast<uint64_t>(pull.duration_seconds));
    const bool has_clock = pull.ended_at_unix_s != 0;
    const std::string clock = format_clock_time(pull.ended_at_unix_s);

    if (ImGui::IsItemHovered()) {
        const char* plural = pull.death_count == 1 ? "" : "s";
        if (has_clock) {
            ImGui::SetTooltip("Pull %s - %s\nEnded %s - %s - %zu death%s", number.c_str(),
                              outcome.label, clock.c_str(), duration.c_str(), pull.death_count, plural);
        } else {
            ImGui::SetTooltip("Pull %s - %s\n%s - %zu death%s", number.c_str(), outcome.label,
                              duration.c_str(), pull.death_count, plural);
        }
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float line_h = ImGui::GetTextLineHeight();
    const float text_y = pos.y + (row_h - line_h) * 0.5f;
    const float gap = m(8.0f);
    const float dot_r = m(3.0f);

    float x = pos.x;
    dl->AddCircleFilled(ImVec2(x + dot_r, text_y + line_h * 0.5f), dot_r, outcome.color);
    x += dot_r * 2.0f + m(6.0f);
    dl->AddText(ImVec2(x, text_y), selected ? colors::TextPrimary : colors::TextMuted, number.c_str());
    x += number_w + gap;

    const float duration_w = ImGui::CalcTextSize(duration.c_str()).x;
    if (x + duration_w > right) return clicked;
    dl->AddText(ImVec2(x, text_y), selected ? colors::TextPrimary : colors::TextBody, duration.c_str());
    x += duration_w + gap;

    // Deaths outrank the end time, so the time only gets what the deaths leave.
    float room = right - x;
    if (pull.death_count > 0) {
        const std::string deaths = ICON_SKULL " " + std::to_string(pull.death_count);
        const float deaths_w = ImGui::CalcTextSize(deaths.c_str()).x;
        if (deaths_w > room) return clicked;
        dl->AddText(ImVec2(x, text_y), colors::DangerLight, deaths.c_str());
        room -= deaths_w + gap;
    }
    const float clock_w = ImGui::CalcTextSize(clock.c_str()).x;
    if (has_clock && clock_w <= room) {
        dl->AddText(ImVec2(right - clock_w, text_y), colors::TextDim, clock.c_str());
    }
    return clicked;
}

/// Clearing drops every archived pull, so it asks first.
void render_clear_history_popup(AppState& app_state, size_t pull_count) {
    if (!ImGui::BeginPopupModal("##ConfirmClearPulls", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        return;
    }
    icon_chip(ICON_WARNING, colors::Danger, metrics::ChipSizeLg);
    ImGui::SameLine(0.0f, m(10.0f));
    ImGui::BeginGroup();
    const std::string title = pull_count == 1
        ? std::string("Clear the archived pull?")
        : "Clear all " + std::to_string(pull_count) + " archived pulls?";
    ImGui::PushFont(bold_font());
    text_colored_u32(colors::TextPrimary, "%s", title.c_str());
    ImGui::PopFont();
    text_colored_u32(colors::TextDim, "%s", "The live encounter is kept.");
    ImGui::EndGroup();
    ImGui::Dummy(ImVec2(0.0f, m(8.0f)));

    if (button(ICON_TRASH "  Clear history", ButtonKind::Danger, ButtonSize::Medium)) {
        app_state.clear_pull_history();
        s_selected_pull_id = 0;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine(0.0f, m(8.0f));
    if (button("Cancel", ButtonKind::Secondary, ButtonSize::Small)) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/// Label over the archive, with the action that clears it on the same line.
void render_history_header(bool has_pulls) {
    const float icon_w = ImGui::GetFrameHeight();
    const float start_y = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(start_y + (icon_w - ImGui::GetTextLineHeight()) * 0.5f);
    text_colored_u32(colors::TextDim, "%s", "PULL HISTORY");
    ImGui::SameLine();
    right_align(icon_w);
    ImGui::SetCursorPosY(start_y);
    ImGui::BeginDisabled(!has_pulls);
    if (button(ICON_TRASH "##ClearPulls", ButtonKind::Danger, ButtonSize::Icon)) {
        ImGui::OpenPopup("##ConfirmClearPulls");
    }
    ImGui::EndDisabled();
    if (has_pulls && ImGui::IsItemHovered()) ImGui::SetTooltip("Clear pull history");
}

/// The one place a pull is chosen: live at the top, then the archive grouped by
/// duty. The tables beside it always show whatever is selected here.
void render_pull_rail(AppState& app_state, const std::vector<meter::PullHistoryEntry>& pull_history,
                      bool is_live, float width) {
    CardOptions opts{};
    begin_card("##PullRail", ImVec2(width, fill_h(0.0f)), opts);

    const bool live_active = s_live_summary.state == meter::EncounterState::InCombat;
    const Badge live_badge = live_active ? Badge{"Active", colors::SuccessLight}
                                         : Badge{"Idle", colors::TextDim};
    if (rail_row("Live##LivePull", is_live, live_badge)) {
        s_selected_pull_id = 0;
    }
    ImGui::Dummy(ImVec2(0.0f, m(6.0f)));
    render_history_header(!pull_history.empty());
    ImGui::Dummy(ImVec2(0.0f, m(2.0f)));

    if (pull_history.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        text_colored_u32(colors::TextFaint, "%s", "Finished pulls appear here.");
        ImGui::PopTextWrapPos();
    } else {
        ImGui::BeginChild("##PullList", ImVec2(0.0f, fill_h(0.0f)), ImGuiChildFlags_None);

        // Sized to the widest number so every row's duration starts in one column.
        uint64_t max_id = 0;
        for (const auto& pull : pull_history) max_id = std::max(max_id, pull.encounter_id);
        const float number_w = ImGui::CalcTextSize(("#" + std::to_string(max_id)).c_str()).x;

        const auto selected_idx = find_pull_index(pull_history, s_selected_pull_id);
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
            ImGui::PushStyleColor(ImGuiCol_Text, v4(colors::TextMuted));
            const bool open = ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", group.label.c_str());
            if (open) {
                for (size_t idx : group.pulls) {
                    const auto& pull = pull_history[idx];
                    if (pull_row(pull, s_selected_pull_id == pull.encounter_id, number_w)) {
                        s_selected_pull_id = pull.encounter_id;
                    }
                }
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    render_clear_history_popup(app_state, pull_history.size());
    end_card();
}

void render_top_bar(AppState& app_state, const meter::EncounterSummary& summary,
                    const meter::PullHistoryEntry* archived) {
    CardOptions opts{};
    // Auto-height: the stat run and the actions wrap onto further lines on a
    // narrow window rather than being cut off by a fixed height.
    opts.auto_height = true;
    begin_card("##CombatTopBar", ImVec2(0.0f, 0.0f), opts);

    // What the tables below are showing, since the picker sits beside them.
    std::string title = "Live encounter";
    if (archived != nullptr) {
        std::string zone = meter::zone_label(archived->zone_id, archived->zone_name);
        if (zone.empty()) zone = "Unknown zone";
        title = zone + "  -  Pull #" + std::to_string(archived->encounter_id);
    }
    ImGui::PushFont(bold_font());
    text_colored_u32(colors::TextPrimary, "%s", title.c_str());
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0.0f, m(2.0f)));

    const float row_y = ImGui::GetCursorPosY();

    // Label-over-value pairs rather than a pipe-separated run of text: the numbers
    // are the point of this bar and need to read first. Each pair wraps to the next
    // line once it no longer fits, instead of running off the card.
    const float stat_w = m(96.0f);
    // Each pair is a group, and a group ends its line, so the pairs on one line
    // are re-aligned to that line's top rather than to the bar's first row.
    float line_top = row_y;
    bool first = true;
    const auto stat = [&](const char* label, const std::string& value, uint32_t color) {
        if (!first && !same_line_if_room(stat_w, 20.0f)) {
            line_top = ImGui::GetCursorPosY();
        }
        first = false;
        ImGui::BeginGroup();
        ImGui::SetCursorPosY(line_top);
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
    stat("DEATHS", std::to_string(summary.deaths.size()),
         summary.deaths.empty() ? colors::TextPrimary : colors::DangerLight);

    const Badge state = encounter_state_badge(summary.state);
    const char* reset_label = ICON_RESET "  Reset encounter";
    const float actions_w = pill_width(state.label) + m(10.0f) +
                            button_width(reset_label, ButtonSize::Medium);
    if (same_line_if_room(actions_w, metrics::Gutter)) {
        right_align(actions_w);
        ImGui::SetCursorPosY(line_top + m(3.0f));
    }
    // The pill is shorter than the button, so it is centred on it.
    const float actions_y = ImGui::GetCursorPosY();
    ImGui::SetCursorPosY(actions_y + (m(metrics::ButtonH) - pill_height()) * 0.5f);
    pill(state.label, state.color);
    ImGui::SameLine(0.0f, m(10.0f));
    ImGui::SetCursorPosY(actions_y);
    if (button(reset_label, ButtonKind::Danger, ButtonSize::Medium)) {
        app_state.reset_encounter();
        s_selected_pull_id = 0;
        s_selected_drilldown_entity = 0;
    }

    end_card();
}

/// Job cell shared by the damage and healing tables.
void combatant_job_cell(const meter::CombatantStats& c) {
    job_badge(c.job, c.actor_type == meter::ActorType::LimitBreak);
}

/// A row's colour, from the same table as its Job badge, Limit Break included.
uint32_t combatant_color(const meter::CombatantStats& c) {
    const auto style = common::ui::combatant_style(c.job, c.actor_type == meter::ActorType::LimitBreak);
    return colors::with_alpha(style.rgb, 1.0f);
}

/// Name cell shared by the damage and healing tables: a row-spanning selectable
/// that drives the drilldown panel.
void combatant_name_cell(const meter::CombatantStats& c, const char* id_prefix) {
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
    const auto sizing = table_sizing(760.0f, kCombatTableFlags);
    if (!ImGui::BeginTable("##DamageRankingTable", 10, sizing.flags, ImVec2(0.0f, height))) return;

    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, m(30.0f));
    ImGui::TableSetupColumn("Job", ImGuiTableColumnFlags_WidthFixed, m(52.0f));
    ImGui::TableSetupColumn("Combatant", sizing.flex_flags(), sizing.flex_width(150.0f, 1.0f));
    ImGui::TableSetupColumn("DPS", ImGuiTableColumnFlags_WidthFixed, m(90.0f));
    ImGui::TableSetupColumn("Damage", ImGuiTableColumnFlags_WidthFixed, m(90.0f));
    ImGui::TableSetupColumn("Share", ImGuiTableColumnFlags_WidthFixed, m(65.0f));
    ImGui::TableSetupColumn("Crit %", ImGuiTableColumnFlags_WidthFixed, m(65.0f));
    ImGui::TableSetupColumn("DH %", ImGuiTableColumnFlags_WidthFixed, m(65.0f));
    ImGui::TableSetupColumn("CDH %", ImGuiTableColumnFlags_WidthFixed, m(65.0f));
    ImGui::TableSetupColumn("Deaths", ImGuiTableColumnFlags_WidthFixed, m(60.0f));
    ImGui::TableSetupScrollFreeze(0, 1);
    combat_table_headers_row();

    int rank = 1;
    for (const meter::CombatantStats* cp : combatants) {
        const auto& c = *cp;
        if (!c.is_friendly() && c.dps == 0.0) continue;

        ImGui::TableNextRow();
        row_progress_bar(static_cast<float>(c.dps / top_dps), combatant_color(c));

        ImGui::TableSetColumnIndex(0);
        text_colored_u32(colors::TextMuted, "%d", rank++);

        ImGui::TableSetColumnIndex(1);
        combatant_job_cell(c);

        ImGui::TableSetColumnIndex(2);
        combatant_name_cell(c, "D");

        ImGui::TableSetColumnIndex(3);
        ImGui::PushFont(bold_font());
        text_colored_u32(combatant_color(c), "%s", format_dps(c.dps).c_str());
        ImGui::PopFont();

        ImGui::TableSetColumnIndex(4);
        text_colored_u32(colors::TextBody, "%s", format_damage(c.total_damage).c_str());

        ImGui::TableSetColumnIndex(5);
        text_colored_u32(colors::TextMuted, "%s", format_percentage(c.damage_share_pct).c_str());

        ImGui::TableSetColumnIndex(6);
        text_colored_u32(colors::TextMuted, "%s", format_percentage(c.hits.crit_rate()).c_str());

        ImGui::TableSetColumnIndex(7);
        text_colored_u32(colors::TextMuted, "%s", format_percentage(c.hits.dh_rate()).c_str());

        ImGui::TableSetColumnIndex(8);
        text_colored_u32(colors::TextMuted, "%s", format_percentage(c.hits.cdh_rate()).c_str());

        ImGui::TableSetColumnIndex(9);
        text_colored_u32(c.deaths > 0 ? colors::DangerLight : colors::TextFaint, "%u", c.deaths);
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
    const auto sizing = table_sizing(680.0f, kCombatTableFlags);
    if (!ImGui::BeginTable("##HealingRankingTable", 7, sizing.flags, ImVec2(0.0f, height))) return;

    ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, m(30.0f));
    ImGui::TableSetupColumn("Job", ImGuiTableColumnFlags_WidthFixed, m(52.0f));
    ImGui::TableSetupColumn("Combatant", sizing.flex_flags(), sizing.flex_width(150.0f, 1.0f));
    ImGui::TableSetupColumn("HPS", ImGuiTableColumnFlags_WidthFixed, m(90.0f));
    ImGui::TableSetupColumn("Total heal", ImGuiTableColumnFlags_WidthFixed, m(100.0f));
    ImGui::TableSetupColumn("Effective", ImGuiTableColumnFlags_WidthFixed, m(110.0f));
    ImGui::TableSetupColumn("Overheal", ImGuiTableColumnFlags_WidthFixed, m(90.0f));
    ImGui::TableSetupScrollFreeze(0, 1);
    combat_table_headers_row();

    int rank = 1;
    for (const meter::CombatantStats* cp : combatants) {
        const auto& c = *cp;
        if (!c.is_friendly() && c.hps == 0.0) continue;

        ImGui::TableNextRow();
        row_progress_bar(static_cast<float>(c.hps / top_hps), colors::Success);

        ImGui::TableSetColumnIndex(0);
        text_colored_u32(colors::TextMuted, "%d", rank++);

        ImGui::TableSetColumnIndex(1);
        combatant_job_cell(c);

        ImGui::TableSetColumnIndex(2);
        combatant_name_cell(c, "H");

        ImGui::TableSetColumnIndex(3);
        ImGui::PushFont(bold_font());
        text_colored_u32(colors::SuccessLight, "%s", format_dps(c.hps).c_str());
        ImGui::PopFont();

        ImGui::TableSetColumnIndex(4);
        text_colored_u32(colors::TextBody, "%s", format_damage(c.total_healing).c_str());

        ImGui::TableSetColumnIndex(5);
        text_colored_u32(colors::TextBody, "%s", format_damage(c.effective_healing).c_str());

        ImGui::TableSetColumnIndex(6);
        text_colored_u32(colors::TextMuted, "%s", format_percentage(c.overheal_pct()).c_str());
    }

    ImGui::EndTable();
}

// ---------------------------------------------------------------------------
// Settings sections. Order and headings are the same for every plugin: what the
// plugin does, then the in-game overlay, then how it is displayed, then the
// destructive actions.
// ---------------------------------------------------------------------------

void render_plugin_section(AppState& app_state) {
    begin_settings_card("##MeterPluginCard", ICON_SLIDERS, "METER BEHAVIOUR", colors::Accent);

    bool party_only = cfg_bool(METER, "party_only", false);
    if (setting_toggle("Party members only", "Exclude everyone outside your party.", &party_only)) {
        cfg_store(METER, "party_only", party_only);
        app_state.send_combat_overlay_party_only(party_only);
    }

    bool track_vitals = cfg_bool(METER, "track_vitals", true);
    if (setting_toggle("Track deaths, buffs and debuffs",
                       "Reads party HP and status lists 4 times a second. Off, the Deaths and "
                       "Buffs & Debuffs tabs stay empty.", &track_vitals)) {
        cfg_store(METER, "track_vitals", track_vitals);
        app_state.send_combat_track_vitals(track_vitals);
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

    end_settings_card();
}

void render_overlay_section(AppState& app_state) {
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

    end_card();
}

void render_display_section(AppState& app_state) {
    begin_settings_card("##MeterDisplayCard", ICON_CHECKLIST, "METER DISPLAY", colors::Violet);

    static const char* metric_modes[] = { "Damage", "Healing" };
    int metric = cfg_int(METER, "overlay_metric", 0);
    begin_setting_row("Meter metric", "Which table the in-game meter draws.");
    if (ImGui::Combo("##meter_metric", &metric, metric_modes, 2)) {
        cfg_store(METER, "overlay_metric", metric);
        app_state.send_combat_overlay_metric(static_cast<uint32_t>(metric));
    }
    end_setting_row();

    bool show_bars = cfg_bool(METER, "show_bars", true);
    if (setting_toggle("Job-coloured row bars", "Draws a role-tinted bar behind each row.", &show_bars)) {
        cfg_store(METER, "show_bars", show_bars);
        app_state.send_combat_show_bars(show_bars);
    }

    bool col_share = cfg_bool(METER, "show_col_share", true);
    if (setting_toggle("Damage share column", "Percentage of total raid damage.", &col_share)) {
        cfg_store(METER, "show_col_share", col_share);
        app_state.send_combat_column_share(col_share);
    }

    bool col_crit = cfg_bool(METER, "show_col_crit", true);
    if (setting_toggle("Critical hit rate column", "Per-combatant crit percentage.", &col_crit)) {
        cfg_store(METER, "show_col_crit", col_crit);
        app_state.send_combat_column_crit(col_crit);
    }

    bool col_dh = cfg_bool(METER, "show_col_dh", true);
    if (setting_toggle("Direct hit rate column", "Per-combatant direct hit percentage.", &col_dh)) {
        cfg_store(METER, "show_col_dh", col_dh);
        app_state.send_combat_column_dh(col_dh);
    }

    bool col_cdh = cfg_bool(METER, "show_col_cdh", true);
    if (setting_toggle("Critical direct hit column", "Combined crit-and-direct percentage.", &col_cdh)) {
        cfg_store(METER, "show_col_cdh", col_cdh);
        app_state.send_combat_column_cdh(col_cdh);
    }

    end_settings_card();
}

void render_maintenance_section(AppState& app_state) {
    begin_settings_card("##MeterMaintenanceCard", ICON_WRENCH, "MAINTENANCE", colors::Warning);

    if (button(ICON_MOVE "  Reset overlay position", ButtonKind::Secondary, ButtonSize::Large)) {
        app_state.send_combat_reset_overlay_geometry();
    }
    ImGui::Dummy(ImVec2(0.0f, m(6.0f)));
    if (button(ICON_POWER "  End encounter", ButtonKind::Secondary, ButtonSize::Large)) {
        app_state.send_combat_end_encounter();
    }
    ImGui::Dummy(ImVec2(0.0f, m(6.0f)));
    if (button(ICON_TRASH "  Reset all statistics", ButtonKind::Danger, ButtonSize::Large)) {
        app_state.send_combat_reset_stats();
        app_state.reset_encounter();
        app_state.clear_pull_history();
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

/// Which breakdown a ranking tab opens under its table.
enum class Drilldown { None, Damage, Healing };

/// One ability in the breakdown, measured in the tab's own metric.
struct AbilityRow {
    const meter::ActionSummary* act{nullptr};
    uint64_t total{0};
    uint64_t min{0};
    double avg{0.0};
    uint64_t max{0};
    const meter::HitCounts* hits{nullptr};
};

void render_drilldown(const meter::EncounterSummary& summary, Drilldown kind) {
    const meter::CombatantStats* selected = nullptr;
    for (const auto& c : summary.combatants) {
        if (c.entity_id == s_selected_drilldown_entity) {
            selected = &c;
            break;
        }
    }
    if (selected == nullptr) return;

    const bool healing = kind == Drilldown::Healing;
    begin_card("##DrilldownCard", ImVec2(0.0f, fill_h(0.0f)));

    char header[128];
    std::snprintf(header, sizeof(header), "%s BREAKDOWN - %s", healing ? "HEALING" : "ABILITY",
                  selected->name.c_str());
    const char* close_label = ICON_CIRCLE_X "  Close";
    begin_section_header(ICON_CROSSHAIR, header, button_width(close_label, ButtonSize::Small),
                         combatant_color(*selected));
    if (button(close_label, ButtonKind::Secondary, ButtonSize::Small)) {
        s_selected_drilldown_entity = 0;
    }
    end_section_header();

    // Heals are kept apart from damage, so each tab only lists what it ranks.
    std::vector<AbilityRow> rows;
    rows.reserve(selected->actions.size());
    for (const auto& [_, act] : selected->actions) {
        if (healing) {
            if (act.heal_hits == 0) continue;
            rows.push_back({&act, act.effective_healing, act.min_heal, act.average_healing(), act.max_heal,
                            &act.heal_hit_counts});
        } else {
            if (act.damage_hits == 0 && act.hits.miss_hits == 0) continue;
            rows.push_back({&act, act.total_damage, act.min_damage, act.average_damage(), act.max_damage,
                            &act.hits});
        }
    }
    std::sort(rows.begin(), rows.end(),
              [](const AbilityRow& a, const AbilityRow& b) { return a.total > b.total; });

    if (rows.empty()) {
        empty_state(ICON_CROSSHAIR, healing ? "No healing recorded" : "No abilities recorded",
                    healing ? "This combatant has not healed anyone yet."
                            : "This combatant has not landed anything yet.");
        end_card();
        return;
    }

    const auto sizing = table_sizing(620.0f, kCombatTableFlags);
    if (ImGui::BeginTable("##DrilldownTable", 7, sizing.flags, ImVec2(0.0f, fill_h(0.0f)))) {
        ImGui::TableSetupColumn("Action", sizing.flex_flags(), sizing.flex_width(160.0f, 1.0f));
        // Every target of an AoE and every tick counts, so these are hits, not casts.
        ImGui::TableSetupColumn("Hits", ImGuiTableColumnFlags_WidthFixed, m(60.0f));
        ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_WidthFixed, m(90.0f));
        ImGui::TableSetupColumn("Min", ImGuiTableColumnFlags_WidthFixed, m(75.0f));
        ImGui::TableSetupColumn("Avg", ImGuiTableColumnFlags_WidthFixed, m(75.0f));
        ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthFixed, m(75.0f));
        ImGui::TableSetupColumn("Crit %", ImGuiTableColumnFlags_WidthFixed, m(65.0f));
        ImGui::TableSetupScrollFreeze(0, 1);
        combat_table_headers_row();

        const uint64_t top = std::max<uint64_t>(rows.front().total, 1);
        const uint32_t bar_color = healing ? colors::Success : combatant_color(*selected);
        for (const AbilityRow& row : rows) {
            ImGui::TableNextRow();
            row_progress_bar(static_cast<float>(static_cast<double>(row.total) / static_cast<double>(top)),
                             bar_color);

            ImGui::TableSetColumnIndex(0);
            text_colored_u32(colors::TextBody, "%s", row.act->name.c_str());

            ImGui::TableSetColumnIndex(1);
            text_colored_u32(colors::TextMuted, "%llu", static_cast<unsigned long long>(row.act->hit_count));

            ImGui::TableSetColumnIndex(2);
            text_colored_u32(colors::TextPrimary, "%s", format_damage(row.total).c_str());

            ImGui::TableSetColumnIndex(3);
            text_colored_u32(colors::TextMuted, "%s", format_damage(row.min).c_str());

            ImGui::TableSetColumnIndex(4);
            text_colored_u32(colors::TextBody, "%s", format_damage(static_cast<uint64_t>(row.avg)).c_str());

            ImGui::TableSetColumnIndex(5);
            text_colored_u32(colors::TextMuted, "%s", format_damage(row.max).c_str());

            ImGui::TableSetColumnIndex(6);
            // Ticks carry no severity, so a status row has no crit rate to show.
            if (row.hits->rated_hits() == 0) {
                text_colored_u32(colors::TextMuted, "%s", "-");
            } else {
                text_colored_u32(colors::TextMuted, "%s", format_percentage(row.hits->crit_rate()).c_str());
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

/// Rail on the left, the selected pull's top bar and a tab's body on the right. The
/// rail narrows on a small window and its rows drop detail to fit. Only the ranking
/// tabs have the ability drilldown under their table.
template <typename TableFn>
void render_pull_view(AppState& app_state, const meter::EncounterSummary& summary,
                      const std::vector<meter::PullHistoryEntry>& pull_history,
                      std::optional<size_t> selected_index, const char* id, TableFn&& table,
                      Drilldown drilldown) {
    const float avail = ImGui::GetContentRegionAvail().x;
    const float rail_w = avail < m(760.0f) ? m(150.0f)
                                           : std::clamp(avail * 0.15f, m(220.0f), m(260.0f));
    render_pull_rail(app_state, pull_history, !selected_index.has_value(), rail_w);
    ImGui::SameLine(0.0f, m(metrics::Gutter));

    ImGui::BeginChild(id, ImVec2(0.0f, fill_h(0.0f)), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground);
    render_top_bar(app_state, summary, selected_index ? &pull_history[*selected_index] : nullptr);
    ImGui::Dummy(ImVec2(0.0f, m(4.0f)));
    if (drilldown != Drilldown::None) {
        table(summary, ranking_table_height());
        render_drilldown(summary, drilldown);
    } else {
        table(summary, fill_h(0.0f));
    }
    ImGui::EndChild();
}

} // namespace
#endif

void render_view_combat(AppState& app_state) {
#ifdef HAVE_IMGUI
    render_plugin_header(app_state, PluginId::CombatMeter, ICON_SWORDS, "Combat Meter",
                         "Per-pull damage, healing, deaths, buffs and debuffs");
    if (render_plugin_disabled_gate(app_state, PluginId::CombatMeter, "Combat Meter")) {
        return;
    }

    const auto pull_history = app_state.get_pull_history_index();

    // A selected pull that has aged out of the archive falls back to live rather
    // than leaving the tables pointing at whatever took its place.
    const auto selected_index = find_pull_index(pull_history, s_selected_pull_id);
    const bool is_live = !selected_index.has_value();

    // Live is snapshotted on the timer even while an archived pull is open, so
    // the rail's live badge stays current.
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - s_last_snapshot).count() >=
        kSnapshotIntervalMs) {
        s_live_summary = app_state.get_live_summary();
        s_last_snapshot = now;
    }
    if (!is_live && s_cached_pull_id != s_selected_pull_id) {
        // An archived pull never changes, so it is fetched once per selection
        // rather than on the live timer. The id only advances on a hit, or a
        // pull that failed to load would leave the tables showing its
        // predecessor under the new pull's header.
        if (auto pull = app_state.get_pull(*selected_index)) {
            s_selected_pull = std::move(*pull);
            s_cached_pull_id = s_selected_pull_id;
        }
    }

    const meter::EncounterSummary& current_summary = is_live ? s_live_summary : s_selected_pull;

    if (ImGui::BeginTabBar("##CombatTabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem(ICON_SWORDS "  Damage")) {
            ImGui::Dummy(ImVec2(0.0f, m(4.0f)));
            render_pull_view(app_state, current_summary, pull_history, selected_index,
                             "##DamagePane", render_damage_table, Drilldown::Damage);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ICON_HEART "  Healing")) {
            ImGui::Dummy(ImVec2(0.0f, m(4.0f)));
            render_pull_view(app_state, current_summary, pull_history, selected_index,
                             "##HealingPane", render_healing_table, Drilldown::Healing);
            ImGui::EndTabItem();
        }
        const bool tracking_on = cfg_bool(METER, "track_vitals", true);
        if (ImGui::BeginTabItem(ICON_SHIELD "  Damage Taken")) {
            ImGui::Dummy(ImVec2(0.0f, m(4.0f)));
            render_pull_view(app_state, current_summary, pull_history, selected_index,
                             "##TakenPane", render_damage_taken, Drilldown::None);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ICON_SKULL "  Deaths")) {
            ImGui::Dummy(ImVec2(0.0f, m(4.0f)));
            render_pull_view(app_state, current_summary, pull_history, selected_index, "##DeathsPane",
                             [tracking_on](const meter::EncounterSummary& summary, float height) {
                                 render_deaths(summary, height, tracking_on);
                             }, Drilldown::None);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ICON_SPARKLE "  Buffs & Debuffs")) {
            ImGui::Dummy(ImVec2(0.0f, m(4.0f)));
            render_pull_view(app_state, current_summary, pull_history, selected_index, "##StatusPane",
                             [tracking_on](const meter::EncounterSummary& summary, float height) {
                                 render_statuses(summary, height, tracking_on);
                             }, Drilldown::None);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(ICON_SLIDERS "  Settings")) {
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
