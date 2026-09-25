#pragma once

#include "app/app_state.hpp"
#include <string>

namespace hub::app::ui {

/**
 * @brief Loads the desktop fonts and applies the slate theme at a DPI scale.
 *
 * Segoe UI regular and bold come from `windows_dir`\Fonts at 16 px times
 * `dpi_scale`, each with the icon glyphs merged in; ImGui's built-in font stands in
 * for a missing regular face. Call once, right after ImGui::CreateContext().
 */
void setup_fonts_and_theme(const std::string& windows_dir, float dpi_scale);

/**
 * @brief Draws one desktop frame between ImGui::NewFrame() and ImGui::Render():
 * the sidebar and the current view, filling the whole client area.
 */
void render_app_frame(AppState& app_state);

} // namespace hub::app::ui
