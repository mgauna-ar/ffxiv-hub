#pragma once

#include "common/ui/imgui_guard.hpp"
#include "meter/types.hpp"

namespace hub::app::ui {

#ifdef HAVE_IMGUI

/// Casts tab: what each player pressed, their GCD and how long they kept it rolling,
/// and the selected player's casts per action.
void render_casts(const meter::EncounterSummary& summary, float height);

#endif // HAVE_IMGUI

} // namespace hub::app::ui
