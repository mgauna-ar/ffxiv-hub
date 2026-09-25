#include "app/ui/combat_casts.hpp"
#include "app/ui/combat_view_common.hpp"
#include "common/ui/icons.hpp"
#include "app/ui/theme.hpp"
#include "app/ui/widgets.hpp"
#include "hub/game/actions.hpp"
#include "hub/game/limit_break.hpp"
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace hub::app::ui {

#ifdef HAVE_IMGUI
namespace {

/// 0 = whoever is listed first.
meter::EntityId s_selected_player = 0;

std::string format_rate(double per_minute) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.1f", per_minute);
    return buf;
}

std::string format_gcd(double seconds) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.2f s", seconds);
    return buf;
}

const char* action_kind(meter::ActionId action) {
    if (game::is_limit_break_action(action)) return "Limit Break";
    return game::is_gcd_action(action) ? "GCD" : "oGCD";
}

void render_player_table(const std::vector<const meter::CombatantStats*>& players, meter::EntityId shown,
                         float height) {
    const auto sizing = table_sizing(560.0f, 7, kCombatTableFlags);
    if (!ImGui::BeginTable("##CastPlayers", 7, sizing.flags, ImVec2(0.0f, height))) return;
    ImGui::TableSetupColumn("Job", ImGuiTableColumnFlags_WidthFixed, m(52.0f));
    ImGui::TableSetupColumn("Player", sizing.flex_flags(), sizing.flex_width(150.0f, 1.0f));
    ImGui::TableSetupColumn("Casts", ImGuiTableColumnFlags_WidthFixed, m(60.0f));
    ImGui::TableSetupColumn("CPM", ImGuiTableColumnFlags_WidthFixed, m(55.0f));
    ImGui::TableSetupColumn("GCDs", ImGuiTableColumnFlags_WidthFixed, m(55.0f));
    ImGui::TableSetupColumn("GCD", ImGuiTableColumnFlags_WidthFixed, m(65.0f));
    ImGui::TableSetupColumn("Uptime", ImGuiTableColumnFlags_WidthFixed, m(70.0f));
    ImGui::TableSetupScrollFreeze(0, 1);
    combat_table_headers_row();

    for (const meter::CombatantStats* p : players) {
        const bool has_gcd = p->gcd_casts > 0;
        ImGui::TableNextRow();
        row_progress_bar(static_cast<float>(p->gcd_uptime_pct / 100.0), get_job_color_u32(p->job));
        ImGui::TableSetColumnIndex(0);
        job_badge(p->job);
        ImGui::TableSetColumnIndex(1);
        const std::string label = p->name + "##Cast" + std::to_string(p->entity_id);
        if (ImGui::Selectable(label.c_str(), p->entity_id == shown, ImGuiSelectableFlags_SpanAllColumns)) {
            s_selected_player = p->entity_id;
        }
        ImGui::TableSetColumnIndex(2);
        text_colored_u32(colors::TextBody, "%u", p->casts);
        ImGui::TableSetColumnIndex(3);
        text_colored_u32(colors::TextMuted, "%s", format_rate(p->cpm).c_str());
        ImGui::TableSetColumnIndex(4);
        text_colored_u32(colors::TextMuted, "%u", p->gcd_casts);
        ImGui::TableSetColumnIndex(5);
        text_colored_u32(has_gcd ? colors::TextBody : colors::TextFaint, "%s",
                         has_gcd ? format_gcd(p->gcd_estimate_s).c_str() : "-");
        if (has_gcd && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("The GCD this player's casts suggest, for a 2.5 s action.");
        }
        ImGui::TableSetColumnIndex(6);
        ImGui::PushFont(bold_font());
        text_colored_u32(has_gcd ? colors::TextPrimary : colors::TextFaint, "%s",
                         has_gcd ? format_percentage(p->gcd_uptime_pct).c_str() : "-");
        ImGui::PopFont();
        if (has_gcd && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Share of the pull the GCD kept rolling. Time the boss was out\n"
                              "of reach counts against it.");
        }
    }
    ImGui::EndTable();
}

void render_action_table(const meter::CombatantStats& player, double duration_s) {
    std::vector<const meter::ActionSummary*> rows;
    for (const auto& [_, act] : player.actions) {
        if (act.casts > 0) rows.push_back(&act);
    }
    std::sort(rows.begin(), rows.end(), [](const meter::ActionSummary* a, const meter::ActionSummary* b) {
        return a->casts != b->casts ? a->casts > b->casts : a->name < b->name;
    });

    const auto sizing = table_sizing(460.0f, 4, kCombatTableFlags);
    if (!ImGui::BeginTable("##CastActions", 4, sizing.flags, ImVec2(0.0f, fill_h(0.0f)))) return;
    ImGui::TableSetupColumn("Action", sizing.flex_flags(), sizing.flex_width(180.0f, 1.0f));
    ImGui::TableSetupColumn("Kind", ImGuiTableColumnFlags_WidthFixed, m(90.0f));
    ImGui::TableSetupColumn("Casts", ImGuiTableColumnFlags_WidthFixed, m(60.0f));
    ImGui::TableSetupColumn("CPM", ImGuiTableColumnFlags_WidthFixed, m(55.0f));
    ImGui::TableSetupScrollFreeze(0, 1);
    combat_table_headers_row();

    const double minutes = std::max(duration_s, 1.0) / 60.0;
    const uint64_t top = rows.empty() ? 1 : std::max<uint64_t>(rows.front()->casts, 1);
    for (const meter::ActionSummary* act : rows) {
        ImGui::TableNextRow();
        row_progress_bar(static_cast<float>(static_cast<double>(act->casts) / static_cast<double>(top)),
                         get_job_color_u32(player.job));
        ImGui::TableSetColumnIndex(0);
        text_colored_u32(colors::TextBody, "%s", act->name.c_str());
        ImGui::TableSetColumnIndex(1);
        text_colored_u32(colors::TextMuted, "%s", action_kind(act->action_id));
        ImGui::TableSetColumnIndex(2);
        text_colored_u32(colors::TextPrimary, "%llu", static_cast<unsigned long long>(act->casts));
        ImGui::TableSetColumnIndex(3);
        text_colored_u32(colors::TextMuted, "%s",
                         format_rate(static_cast<double>(act->casts) / minutes).c_str());
    }
    ImGui::EndTable();
}

} // namespace

void render_casts(const meter::EncounterSummary& summary, float height) {
    std::vector<const meter::CombatantStats*> players;
    for (const meter::CombatantStats& c : summary.combatants) {
        if (c.casts > 0) players.push_back(&c);
    }
    std::sort(players.begin(), players.end(), [](const meter::CombatantStats* a, const meter::CombatantStats* b) {
        return a->casts != b->casts ? a->casts > b->casts : a->entity_id < b->entity_id;
    });

    if (players.empty()) {
        empty_state(ICON_BOLT, "No casts recorded", "What each player presses appears here once a pull starts.");
        return;
    }

    // A player who is not in this pull any more cannot stay selected.
    const auto selected = std::find_if(players.begin(), players.end(),
        [](const meter::CombatantStats* p) { return p->entity_id == s_selected_player; });
    const meter::CombatantStats& shown = selected != players.end() ? **selected : *players.front();

    const float players_h = std::min(height * 0.42f,
        (static_cast<float>(players.size()) + 1.5f) * (ImGui::GetTextLineHeight() + m(10.0f)));
    render_player_table(players, shown.entity_id, players_h);
    ImGui::Dummy(ImVec2(0.0f, m(6.0f)));

    section_header(ICON_BOLT, shown.name.c_str(), get_job_color_u32(shown.job));
    render_action_table(shown, summary.duration_seconds);
}

#endif // HAVE_IMGUI

} // namespace hub::app::ui
