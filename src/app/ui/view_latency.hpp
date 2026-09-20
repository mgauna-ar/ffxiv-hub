#pragma once

#include "app/app_state.hpp"

namespace hub::app::ui {

/**
 * @brief Renders the dedicated Latency Mitigator view.
 * Features real-time RTT & jitter graph, telemetry metric cards,
 * live rolling action feed, and animation lock safety settings.
 */
void render_view_latency(AppState& app_state);

} // namespace hub::app::ui
