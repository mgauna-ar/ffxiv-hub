#pragma once

#include "app/app_state.hpp"
#include "app/ui/combat_casts.hpp"
#include "app/ui/combat_damage_taken.hpp"
#include "app/ui/combat_deaths.hpp"
#include "app/ui/combat_statuses.hpp"
#include "app/ui/combat_timeline.hpp"
#include "meter/types.hpp"
#include <chrono>
#include <cstdint>

namespace hub::app::ui {

/// What the Combat Meter view remembers between frames: the pull and combatant
/// picked, the summaries it last fetched, and each tab's own state.
struct CombatViewState {
    /// Keyed on encounter_id, not on a position in the archive: the history deque
    /// evicts from the front at capacity, so an index stops meaning the same pull.
    /// 0 = live encounter.
    uint64_t selected_pull_id{0};
    uint32_t selected_drilldown_entity{0};

    /// A full EncounterSummary carries every combatant's per-action breakdown, so
    /// refetching one per frame means hundreds of map and string allocations under
    /// the combat mutex. Snapshot on a timer instead, like the in-game overlay does.
    meter::EncounterSummary live_summary;
    meter::EncounterSummary selected_pull;
    uint64_t cached_pull_id{0};
    std::chrono::steady_clock::time_point last_snapshot{};

    DamageTakenTabState damage_taken;
    DeathsTabState deaths;
    StatusesTabState statuses;
    CastsTabState casts;
    TimelineTabState timeline;
};

/**
 * @brief Renders the dedicated Combat Meter analytical inspector view.
 * Features live/history damage & healing rankings with job progress bars,
 * per-action player drilldowns, and in-game overlay configuration.
 */
void render_view_combat(AppState& app_state, CombatViewState& state);

} // namespace hub::app::ui
