#pragma once

#include "common/ui/imgui_guard.hpp"
#include "meter/types.hpp"

namespace hub::app::ui {

/// What the Casts tab remembers between frames.
struct CastsTabState {
    /// 0 = whoever is listed first.
    meter::EntityId selected_player{0};
};

#ifdef HAVE_IMGUI

/// Casts tab: what each player pressed, their GCD and how long they kept it rolling,
/// and the selected player's casts per action.
void render_casts(CastsTabState& state, const meter::EncounterSummary& summary, float height);

#endif // HAVE_IMGUI

} // namespace hub::app::ui
