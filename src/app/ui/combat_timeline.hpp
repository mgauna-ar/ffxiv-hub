#pragma once

#include "common/ui/imgui_guard.hpp"
#include "meter/types.hpp"
#include <cstdint>

namespace hub::app {
class AppState;
}

namespace hub::app::ui {

#ifdef HAVE_IMGUI

/// Timeline tab: each party member's damage, healing or damage taken over the pull,
/// smoothed, over the raid buff windows, with deaths marked. `encounter_id` 0 is the
/// live pull.
void render_timeline(AppState& app_state, const meter::EncounterSummary& summary, uint64_t encounter_id,
                     float height);

#endif // HAVE_IMGUI

} // namespace hub::app::ui
