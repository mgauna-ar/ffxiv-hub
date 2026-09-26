#pragma once

#include "common/ui/overlay_config.hpp"
#include "hub/types.hpp"

namespace hub::app {
class AppState;
}

namespace hub::app::ui {

/// Everything that differs between one overlay's settings block and another's.
struct OverlaySettingsOptions {
    const char* section{nullptr};   ///< config.json section, e.g. "combat_meter"
    PluginId plugin{PluginId::Core};
    const char* visible_label{"Show In-Game Overlay"};
    const char* locked_label{"Lock Overlay Position & Size"};
    const char* opacity_label{"Background Opacity"};
    const char* scale_label{"UI Scale"};
    float min_opacity{0.2f};
    float max_opacity{1.0f};
    float min_scale{0.7f};
    float max_scale{2.0f};
    hub::ui::OverlayConfig defaults{};
    /// Overlays that size themselves have nothing useful to scale.
    bool show_scale{true};
};

/// Renders the controls every overlay shares - visibility, lock, click-through,
/// opacity, scale, and the visibility conditions - reading and writing the
/// canonical config keys and mirroring each change to the payload.
void render_overlay_settings(AppState& app_state, const OverlaySettingsOptions& opts);

} // namespace hub::app::ui
