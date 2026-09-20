#pragma once

#include "app/app_state.hpp"

namespace hub::app::ui {

/**
 * @brief Renders the dedicated Combat Meter analytical inspector view.
 * Features live/history damage & healing rankings with job progress bars,
 * per-action player drilldowns, and in-game overlay configuration.
 */
void render_view_combat(AppState& app_state);

} // namespace hub::app::ui
