#pragma once

#include "app/app_state.hpp"
#include "app/ui/view_combat.hpp"
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
 * @brief The desktop window's contents: the sidebar and the current view, and what
 * each view remembers between frames. The views keep no state of their own, so two
 * frames never share a selection or a cached snapshot.
 */
class AppFrame {
public:
    /// Draws one frame between ImGui::NewFrame() and ImGui::Render(), filling the
    /// whole client area.
    void render(AppState& app_state);

private:
    CombatViewState m_combat;
};

} // namespace hub::app::ui
