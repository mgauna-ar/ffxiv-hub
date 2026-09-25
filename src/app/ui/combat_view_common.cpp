#include "app/ui/combat_view_common.hpp"
#include "app/ui/config_binding.hpp"
#include "app/ui/theme.hpp"
#include "app/ui/widgets.hpp"
#include <cstdio>

namespace hub::app::ui {

#ifdef HAVE_IMGUI

void combat_table_headers_row() {
    ImGui::PushStyleColor(ImGuiCol_Text, v4(colors::TextDim));
    ImGui::TableHeadersRow();
    ImGui::PopStyleColor();
}

meter::DpsMetric selected_dps_metric() {
    return meter::dps_metric_from(static_cast<uint32_t>(cfg_int("combat_meter", "dps_metric", 0)));
}

SummaryNames::SummaryNames(const meter::EncounterSummary& summary) {
    m_entries.reserve(summary.combatants.size() + summary.names.size());
    for (const meter::ActorLabel& label : summary.names) {
        m_entries[label.entity] = Entry{label.name, label.job};
    }
    // Combatant rows win: their names were refreshed through the whole pull.
    for (const meter::CombatantStats& c : summary.combatants) {
        m_entries[c.entity_id] = Entry{c.name, c.job};
    }
}

const std::string& SummaryNames::name(meter::EntityId id) const {
    static const std::string kNone = "-";
    static const std::string kUnknown = "Unknown";
    if (id == 0) return kNone;
    auto it = m_entries.find(id);
    return it != m_entries.end() ? it->second.name : kUnknown;
}

game::Job SummaryNames::job(meter::EntityId id) const {
    auto it = m_entries.find(id);
    return it != m_entries.end() ? it->second.job : game::Job::None;
}

std::string ability_label(meter::ActionId action_key, bool heal) {
    if ((action_key & meter::STATUS_ACTION_KEY_OFFSET) != 0) {
        return meter::status_id_to_name(action_key & ~meter::STATUS_ACTION_KEY_OFFSET) + (heal ? " (HoT)" : " (DoT)");
    }
    return meter::action_id_to_name(action_key);
}

std::string format_seconds(double seconds) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f s", seconds);
    return buf;
}

std::string status_label(uint16_t status_id) {
    return meter::status_id_to_name(status_id);
}

void vitals_empty_state(bool tracking_on, const char* icon, const char* title, const char* hint) {
    if (tracking_on) {
        empty_state(icon, title, hint);
    } else {
        empty_state(icon, "Tracking is off",
                    "Turn on \"Track deaths, buffs and debuffs\" in Settings to fill this tab.");
    }
}

#endif // HAVE_IMGUI

} // namespace hub::app::ui
