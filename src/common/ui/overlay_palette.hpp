#pragma once

#include "common/ui/imgui_guard.hpp"

namespace hub::common::ui {

/// A colour as ImGui's float channels, without alpha: the in-game overlays give the
/// alpha where they use it, since one colour is drawn at several.
struct Rgb {
    float r;
    float g;
    float b;
};

/// The in-game overlays' colours: the overlay host's ImGui style, the combat meter
/// and the ping HUD. The desktop app has its own, in `app/ui/theme.hpp`.
namespace overlay_colors {

// Surfaces, darkest first.
inline constexpr Rgb SurfaceSunken{0.06f, 0.07f, 0.10f};
inline constexpr Rgb Background{0.08f, 0.09f, 0.12f};
inline constexpr Rgb Surface{0.10f, 0.12f, 0.16f};
inline constexpr Rgb SurfaceRaised{0.12f, 0.14f, 0.19f};

// Frames, headers, buttons and tabs.
inline constexpr Rgb FrameHovered{0.18f, 0.22f, 0.30f};
inline constexpr Rgb FrameActive{0.22f, 0.27f, 0.38f};
inline constexpr Rgb HeaderHovered{0.24f, 0.30f, 0.40f};
inline constexpr Rgb HeaderActive{0.30f, 0.37f, 0.50f};
inline constexpr Rgb Control{0.15f, 0.18f, 0.24f};
inline constexpr Rgb ControlHovered{0.22f, 0.27f, 0.36f};
inline constexpr Rgb ControlActive{0.28f, 0.34f, 0.46f};
inline constexpr Rgb TabHovered{0.20f, 0.24f, 0.32f};
inline constexpr Rgb TabActive{0.15f, 0.18f, 0.25f};

// Borders, separators and grips.
inline constexpr Rgb Border{0.20f, 0.23f, 0.30f};
inline constexpr Rgb BorderLight{0.15f, 0.17f, 0.22f};
inline constexpr Rgb SeparatorHovered{0.30f, 0.35f, 0.45f};
inline constexpr Rgb SeparatorActive{0.40f, 0.47f, 0.60f};
inline constexpr Rgb Grip{0.25f, 0.28f, 0.36f};
inline constexpr Rgb ScrollbarGrabHovered{0.35f, 0.39f, 0.50f};
inline constexpr Rgb ScrollbarGrabActive{0.45f, 0.50f, 0.65f};
inline constexpr Rgb ResizeGripHovered{0.35f, 0.40f, 0.52f};
inline constexpr Rgb ResizeGripActive{0.45f, 0.52f, 0.68f};

// Accent and text.
inline constexpr Rgb Accent{0.23f, 0.51f, 0.96f};
inline constexpr Rgb AccentBright{0.30f, 0.58f, 1.00f};
inline constexpr Rgb Text{0.92f, 0.93f, 0.95f};
inline constexpr Rgb TextDisabled{0.45f, 0.48f, 0.55f};
/// Idle and unknown: the meter's IDLE label and the HUD dot with no ping.
inline constexpr Rgb Muted{0.55f, 0.60f, 0.70f};
inline constexpr Rgb Black{0.00f, 0.00f, 0.00f};
inline constexpr Rgb White{1.00f, 1.00f, 1.00f};

// Ping HUD: the dot's grades, its text, and the spike state.
inline constexpr Rgb PingGood{0.20f, 0.85f, 0.40f};
inline constexpr Rgb PingFair{0.12f, 0.79f, 0.59f};
inline constexpr Rgb PingPoor{0.95f, 0.70f, 0.20f};
inline constexpr Rgb PingBad{0.95f, 0.25f, 0.25f};
inline constexpr Rgb HudText{0.92f, 0.94f, 0.98f};
inline constexpr Rgb HudSecondaryText{0.75f, 0.80f, 0.90f};
inline constexpr Rgb SpikeBackground{0.25f, 0.12f, 0.02f};
inline constexpr Rgb SpikeDot{1.00f, 0.45f, 0.10f};
inline constexpr Rgb SpikeText{1.00f, 0.70f, 0.20f};
inline constexpr Rgb SpikeBorder{0.95f, 0.55f, 0.15f};

// Combat meter's top bar and tables.
inline constexpr Rgb Live{0.95f, 0.30f, 0.35f};
inline constexpr Rgb Timer{0.80f, 0.85f, 0.95f};
inline constexpr Rgb Dps{0.96f, 0.50f, 0.20f};
inline constexpr Rgb Hps{0.10f, 0.80f, 0.40f};
inline constexpr Rgb TotalText{0.70f, 0.75f, 0.85f};
inline constexpr Rgb HighOverheal{0.95f, 0.35f, 0.35f};

} // namespace overlay_colors

#ifdef HAVE_IMGUI
/// An overlay colour at `alpha`, for ImGui.
[[nodiscard]] inline ImVec4 rgba(Rgb color, float alpha = 1.0f) noexcept {
    return ImVec4(color.r, color.g, color.b, alpha);
}
#endif

} // namespace hub::common::ui
