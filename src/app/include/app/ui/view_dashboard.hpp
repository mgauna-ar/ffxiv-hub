#pragma once

#include "app/app_state.hpp"

namespace hub::app::ui {

/**
 * @brief Renders the generic, plugin-agnostic Dashboard view.
 * Displays FFXIV game process status, IPC Named Pipe server telemetry,
 * dynamic registered plugins registry, and hub diagnostic actions.
 */
void render_view_dashboard(AppState& app_state);

} // namespace hub::app::ui
