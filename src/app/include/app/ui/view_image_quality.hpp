#pragma once

#include "app/app_state.hpp"

namespace hub::app::ui {

/// The Image Quality view: what the game renders at under DLSS, the jitter it
/// uses, and the plugin's settings.
void render_view_image_quality(AppState& app_state);

} // namespace hub::app::ui
