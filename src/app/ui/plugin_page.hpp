#pragma once

#include "app/app_state.hpp"
#include "common/ui/imgui_guard.hpp"
#include <cstddef>
#include <functional>

namespace hub::app::ui {

#ifdef HAVE_IMGUI

/// One status badge beside a plugin's page title: text plus its color. An empty
/// text means the plugin has nothing extra to say beyond enabled/disabled.
struct PluginStatus {
    const char* text{nullptr};
    uint32_t color{0};
};

/// Page title band for a plugin view: icon, title, subtitle, the plugin's own
/// status badge, and the master enable switch. Every plugin view opens with this
/// call, so the switch is always in the same place.
void render_plugin_header(AppState& app_state, PluginId id, const char* icon,
                          const char* title, const char* subtitle,
                          const PluginStatus& status = {});

/// Draws the "this plugin is switched off" panel and returns true when the
/// caller should render nothing else. The panel carries the only control that
/// can bring the plugin back.
[[nodiscard]] bool render_plugin_disabled_gate(AppState& app_state, PluginId id,
                                               const char* name);

/// One card in a plugin's settings tab. The order these are listed in is the
/// order the user sees, and it is the same order for every plugin.
struct SettingsSection {
    std::function<void()> body;
};

/// Lays settings sections out in a responsive grid: two balanced columns when
/// there is room, one column otherwise. Replaces the hand-rolled BeginChild pair
/// each plugin view used to carry, which is how the two drifted apart.
void render_settings_grid(const SettingsSection* sections, size_t count);

/// Opens the standard "Plugin" section every settings tab starts with: the
/// master enable switch, followed by whatever the plugin adds. Always paired
/// with end_settings_card().
void begin_settings_card(const char* id, const char* icon, const char* label,
                         uint32_t accent);
void end_settings_card();

#endif // HAVE_IMGUI

} // namespace hub::app::ui
