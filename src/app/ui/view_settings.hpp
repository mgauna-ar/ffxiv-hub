#pragma once

#include "app/app_state.hpp"

namespace hub::app::ui {

/**
 * @brief Renders the Hub Settings view.
 * Handles Windows auto-start configuration, live log inspection,
 * and configuration file management.
 */
void render_view_settings(AppState& app_state);

} // namespace hub::app::ui
