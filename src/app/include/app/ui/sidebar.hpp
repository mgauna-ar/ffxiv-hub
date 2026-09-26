#pragma once

#include "app/app_state.hpp"

namespace hub::app::ui {

/**
 * @brief Renders the left navigation sidebar with responsive view switching,
 * application branding, and live FFXIV connection status.
 */
void render_sidebar(AppState& app_state);

} // namespace hub::app::ui
