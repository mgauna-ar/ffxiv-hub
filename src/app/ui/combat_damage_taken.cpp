#include "app/ui/combat_damage_taken.hpp"
#include "app/ui/combat_view_common.hpp"
#include "common/ui/icons.hpp"
#include "app/ui/theme.hpp"
#include "app/ui/widgets.hpp"
#include <algorithm>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace hub::app::ui {

#ifdef HAVE_IMGUI
namespace {

/// 0 = every player.
meter::EntityId s_selected_target = 0;

void render_player_table(const std::vector<const meter::CombatantStats*>& players,
                         const std::unordered_map<meter::EntityId, uint64_t>& hits_by_target,
                         uint64_t total_taken, float height) {
    const auto sizing = table_sizing(520.0f, kCombatTableFlags);
    if (!ImGui::BeginTable("##TakenPlayers", 5, sizing.flags, ImVec2(0.0f, height))) return;
    ImGui::TableSetupColumn("Job", ImGuiTableColumnFlags_WidthFixed, m(52.0f));
    ImGui::TableSetupColumn("Player", sizing.flex_flags(), sizing.flex_width(150.0f, 1.0f));
    ImGui::TableSetupColumn("Taken", ImGuiTableColumnFlags_WidthFixed, m(90.0f));
    ImGui::TableSetupColumn("Hits", ImGuiTableColumnFlags_WidthFixed, m(65.0f));
    ImGui::TableSetupColumn("Deaths", ImGuiTableColumnFlags_WidthFixed, m(60.0f));
    ImGui::TableSetupScrollFreeze(0, 1);
    combat_table_headers_row();

    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(1);
    if (ImGui::Selectable("All players##TakenAll", s_selected_target == 0, ImGuiSelectableFlags_SpanAllColumns)) {
        s_selected_target = 0;
    }
    ImGui::TableSetColumnIndex(2);
    text_colored_u32(colors::TextPrimary, "%s", format_damage(total_taken).c_str());

    const uint64_t top = std::max<uint64_t>(players.empty() ? 1 : players.front()->damage_taken, 1);
    for (const meter::CombatantStats* p : players) {
        ImGui::TableNextRow();
        row_progress_bar(static_cast<float>(static_cast<double>(p->damage_taken) / static_cast<double>(top)),
                         get_job_color_u32(p->job));
        ImGui::TableSetColumnIndex(0);
        job_badge(p->job);
        ImGui::TableSetColumnIndex(1);
        const std::string label = p->name + "##Taken" + std::to_string(p->entity_id);
        const bool selected = s_selected_target == p->entity_id;
        if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
            s_selected_target = selected ? 0 : p->entity_id;
        }
        ImGui::TableSetColumnIndex(2);
        text_colored_u32(colors::TextBody, "%s", format_damage(p->damage_taken).c_str());
        ImGui::TableSetColumnIndex(3);
        const auto hits = hits_by_target.find(p->entity_id);
        text_colored_u32(colors::TextMuted, "%llu",
                         static_cast<unsigned long long>(hits != hits_by_target.end() ? hits->second : 0));
        ImGui::TableSetColumnIndex(4);
        text_colored_u32(p->deaths > 0 ? colors::DangerLight : colors::TextFaint, "%u", p->deaths);
    }
    ImGui::EndTable();
}

void render_ability_table(const meter::EncounterSummary& summary, const SummaryNames& names, float height) {
    // One row per ability and source: the selected player's own, or summed over everyone.
    std::map<std::tuple<meter::ActionId, meter::EntityId>, meter::DamageTakenRow> rows;
    for (const meter::DamageTakenRow& row : summary.damage_taken) {
        if (s_selected_target != 0 && row.target != s_selected_target) continue;
        meter::DamageTakenRow& sum = rows[{row.action_key, row.source}];
        sum.action_key = row.action_key;
        sum.source = row.source;
        sum.hits += row.hits;
        sum.total += row.total;
        sum.max = std::max(sum.max, row.max);
        sum.deaths_caused += row.deaths_caused;
    }
    std::vector<meter::DamageTakenRow> sorted;
    sorted.reserve(rows.size());
    for (const auto& [key, row] : rows) sorted.push_back(row);
    std::sort(sorted.begin(), sorted.end(), [](const meter::DamageTakenRow& a, const meter::DamageTakenRow& b) {
        return a.total > b.total;
    });

    if (sorted.empty()) {
        empty_state(ICON_SHIELD, "Nothing hit this player", "Pick another player, or All players.");
        return;
    }

    const auto sizing = table_sizing(640.0f, kCombatTableFlags);
    if (!ImGui::BeginTable("##TakenAbilities", 7, sizing.flags, ImVec2(0.0f, height))) return;
    ImGui::TableSetupColumn("Ability", sizing.flex_flags(), sizing.flex_width(160.0f, 1.2f));
    ImGui::TableSetupColumn("Source", sizing.flex_flags(), sizing.flex_width(120.0f, 1.0f));
    ImGui::TableSetupColumn("Hits", ImGuiTableColumnFlags_WidthFixed, m(55.0f));
    ImGui::TableSetupColumn("Total", ImGuiTableColumnFlags_WidthFixed, m(85.0f));
    ImGui::TableSetupColumn("Avg", ImGuiTableColumnFlags_WidthFixed, m(75.0f));
    ImGui::TableSetupColumn("Max", ImGuiTableColumnFlags_WidthFixed, m(75.0f));
    ImGui::TableSetupColumn("Deaths", ImGuiTableColumnFlags_WidthFixed, m(60.0f));
    ImGui::TableSetupScrollFreeze(0, 1);
    combat_table_headers_row();

    const uint64_t top = std::max<uint64_t>(sorted.front().total, 1);
    for (const meter::DamageTakenRow& row : sorted) {
        ImGui::TableNextRow();
        row_progress_bar(static_cast<float>(static_cast<double>(row.total) / static_cast<double>(top)),
                         colors::Danger);
        ImGui::TableSetColumnIndex(0);
        text_colored_u32(colors::TextBody, "%s", ability_label(row.action_key).c_str());
        ImGui::TableSetColumnIndex(1);
        text_colored_u32(colors::TextMuted, "%s", names.name(row.source).c_str());
        ImGui::TableSetColumnIndex(2);
        text_colored_u32(colors::TextMuted, "%llu", static_cast<unsigned long long>(row.hits));
        ImGui::TableSetColumnIndex(3);
        text_colored_u32(colors::TextPrimary, "%s", format_damage(row.total).c_str());
        ImGui::TableSetColumnIndex(4);
        text_colored_u32(colors::TextMuted, "%s",
                         format_damage(row.hits > 0 ? row.total / row.hits : 0).c_str());
        ImGui::TableSetColumnIndex(5);
        text_colored_u32(colors::TextMuted, "%s", format_damage(row.max).c_str());
        ImGui::TableSetColumnIndex(6);
        text_colored_u32(row.deaths_caused > 0 ? colors::DangerLight : colors::TextFaint, "%u", row.deaths_caused);
    }
    ImGui::EndTable();
}

} // namespace

void render_damage_taken(const meter::EncounterSummary& summary, float height) {
    std::vector<const meter::CombatantStats*> players;
    uint64_t total_taken = 0;
    for (const meter::CombatantStats& c : summary.combatants) {
        if (c.is_pet || c.actor_type == meter::ActorType::LimitBreak) continue;
        if (c.damage_taken == 0 && c.deaths == 0) continue;
        players.push_back(&c);
        total_taken += c.damage_taken;
    }
    std::sort(players.begin(), players.end(), [](const meter::CombatantStats* a, const meter::CombatantStats* b) {
        return a->damage_taken > b->damage_taken;
    });

    if (players.empty() && summary.damage_taken.empty()) {
        empty_state(ICON_SHIELD, "No damage taken", "Hits on your party appear here, split by ability.");
        return;
    }

    // A player who is not in this pull any more cannot stay selected.
    if (s_selected_target != 0 && std::none_of(players.begin(), players.end(),
            [](const meter::CombatantStats* p) { return p->entity_id == s_selected_target; })) {
        s_selected_target = 0;
    }

    std::unordered_map<meter::EntityId, uint64_t> hits_by_target;
    for (const meter::DamageTakenRow& row : summary.damage_taken) {
        hits_by_target[row.target] += row.hits;
    }

    const SummaryNames names(summary);
    const float players_h = std::min(height * 0.42f,
        (static_cast<float>(players.size()) + 2.5f) * (ImGui::GetTextLineHeight() + m(10.0f)));
    render_player_table(players, hits_by_target, total_taken, players_h);
    ImGui::Dummy(ImVec2(0.0f, m(6.0f)));

    const std::string heading = s_selected_target == 0
        ? std::string("ALL PLAYERS")
        : names.name(s_selected_target);
    section_header(ICON_CROSSHAIR, heading.c_str(), colors::Danger);
    render_ability_table(summary, names, fill_h(0.0f));
}

#endif // HAVE_IMGUI

} // namespace hub::app::ui
