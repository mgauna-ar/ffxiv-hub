#pragma once

#include "common/ui/imgui_guard.hpp"
#include "meter/types.hpp"

namespace hub::app::ui {

/// What the Deaths tab remembers between frames. A death is picked by who died and
/// when, since the list is rebuilt on every snapshot.
struct DeathsTabState {
    meter::EntityId selected_entity{0};
    double selected_time{-1.0};
};

#ifdef HAVE_IMGUI

/// Deaths tab: every death in the pull with its killing blow and the debuffs the
/// player had, and the recap of the selected one.
void render_deaths(DeathsTabState& state, const meter::EncounterSummary& summary, float height, bool tracking_on);

#endif // HAVE_IMGUI

} // namespace hub::app::ui
