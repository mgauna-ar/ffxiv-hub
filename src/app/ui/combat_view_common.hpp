#pragma once

#include "common/ui/imgui_guard.hpp"
#include "meter/types.hpp"
#include <string>
#include <unordered_map>

namespace hub::app::ui {

#ifdef HAVE_IMGUI

/// Base flags every Combat table shares; table_sizing() picks the sizing policy per
/// table, which is what lets a narrow window scroll instead of crushing the columns.
/// PadOuterX because without outer borders ImGui drops the edge padding and the
/// first and last columns touch the table's sides.
constexpr ImGuiTableFlags kCombatTableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                              ImGuiTableFlags_BordersInnerV |
                                              ImGuiTableFlags_PadOuterX;

/// Header row in the dim header colour every Combat table uses.
void combat_table_headers_row();

/// Names and jobs for the ids a summary's detail rows refer to: its combatants,
/// plus the labels it carries for everyone else, enemies included.
class SummaryNames {
public:
    explicit SummaryNames(const meter::EncounterSummary& summary);

    /// "-" for no id, "Unknown" for an id the summary carries no name for.
    [[nodiscard]] const std::string& name(meter::EntityId id) const;
    [[nodiscard]] game::Job job(meter::EntityId id) const;

private:
    struct Entry {
        std::string name;
        game::Job job{game::Job::None};
    };
    std::unordered_map<meter::EntityId, Entry> m_entries;
};

/// Ability name for a detail row's key; a status tick reads "Dia (DoT)".
[[nodiscard]] std::string ability_label(meter::ActionId action_key);

/// Seconds as "12.3 s".
[[nodiscard]] std::string format_seconds(double seconds);

/// Status name for an id, or "Status <id>" when the sheet has none.
[[nodiscard]] std::string status_label(uint16_t status_id);

/// Empty state for a tab fed by the vitals polling, explaining when it is off.
void vitals_empty_state(bool tracking_on, const char* icon, const char* title, const char* hint);

#endif // HAVE_IMGUI

} // namespace hub::app::ui
