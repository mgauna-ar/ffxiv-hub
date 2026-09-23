#pragma once

#include "common/ui/imgui_guard.hpp"
#include "meter/types.hpp"

namespace hub::app::ui {

#ifdef HAVE_IMGUI

/// Deaths tab: every death in the pull with its killing blow and the debuffs the
/// player had, and the recap of the selected one.
void render_deaths(const meter::EncounterSummary& summary, float height, bool tracking_on);

#endif // HAVE_IMGUI

} // namespace hub::app::ui
