#pragma once

#include "common/ui/imgui_guard.hpp"
#include "meter/types.hpp"

namespace hub::app::ui {

/// What the Damage Taken tab remembers between frames.
struct DamageTakenTabState {
    /// 0 = every player.
    meter::EntityId selected_target{0};
};

#ifdef HAVE_IMGUI

/// Damage Taken tab: who took how much, and what it came from. The ability table
/// follows the selected player, or totals everyone when none is selected.
void render_damage_taken(DamageTakenTabState& state, const meter::EncounterSummary& summary, float height);

#endif // HAVE_IMGUI

} // namespace hub::app::ui
