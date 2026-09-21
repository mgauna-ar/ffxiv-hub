#pragma once

#include "hub/types.hpp"
#include "hub/game/job.hpp"
#include <cstdint>
#include <string>

// Forward declaration to avoid pulling imgui.h into every translation unit that includes this header.
struct ImFont;

namespace hub::app::ui {

namespace colors {

/// Packs a color the way IM_COL32 does (R in the low byte), so a token can be written
/// in readable channel order instead of a hand-assembled hex literal.
constexpr uint32_t rgb(uint32_t r, uint32_t g, uint32_t b, uint32_t a = 255) {
    return (a << 24) | (b << 16) | (g << 8) | r;
}

// Surfaces, darkest to lightest.
constexpr uint32_t Canvas        = rgb(0x0B, 0x0E, 0x14);
constexpr uint32_t SurfaceSunken = rgb(0x09, 0x0C, 0x12);
constexpr uint32_t Surface       = rgb(0x16, 0x1B, 0x26);
constexpr uint32_t SurfaceLow    = rgb(0x11, 0x15, 0x1E);
constexpr uint32_t SurfaceRaised = rgb(0x1C, 0x22, 0x31);
constexpr uint32_t SidebarTop    = rgb(0x0F, 0x13, 0x1C);
constexpr uint32_t SidebarBottom = rgb(0x0A, 0x0D, 0x13);

// Lines.
constexpr uint32_t Border        = rgb(0x23, 0x2B, 0x3D);
constexpr uint32_t BorderStrong  = rgb(0x2A, 0x32, 0x45);
constexpr uint32_t BorderSubtle  = rgb(0x1A, 0x21, 0x30);

// Text.
constexpr uint32_t TextPrimary   = rgb(0xF3, 0xF4, 0xF6);
constexpr uint32_t TextBody      = rgb(0xC9, 0xD1, 0xDE);
constexpr uint32_t TextMuted     = rgb(0x9C, 0xA3, 0xAF);
constexpr uint32_t TextDim       = rgb(0x6B, 0x72, 0x80);
constexpr uint32_t TextFaint     = rgb(0x4B, 0x55, 0x63);
constexpr uint32_t White         = rgb(0xFF, 0xFF, 0xFF);

// Accent.
constexpr uint32_t Accent        = rgb(0x3B, 0x82, 0xF6);
constexpr uint32_t AccentHover   = rgb(0x60, 0xA5, 0xFA);
constexpr uint32_t AccentDeep    = rgb(0x1D, 0x4E, 0xD8);

// Semantic.
constexpr uint32_t Success       = rgb(0x10, 0xB9, 0x81);
constexpr uint32_t SuccessLight  = rgb(0x34, 0xD3, 0x99);
constexpr uint32_t Warning       = rgb(0xF5, 0x9E, 0x0B);
constexpr uint32_t WarningLight  = rgb(0xFB, 0xBF, 0x24);
constexpr uint32_t Danger        = rgb(0xEF, 0x44, 0x44);
constexpr uint32_t DangerLight   = rgb(0xF8, 0x71, 0x71);
constexpr uint32_t Violet        = rgb(0xA8, 0x55, 0xF7);

// FFXIV role palette.
constexpr uint32_t TankBlue      = rgb(0x3B, 0x82, 0xF6);
constexpr uint32_t HealerGreen   = rgb(0x10, 0xB9, 0x81);
constexpr uint32_t MeleeRed      = rgb(0xEF, 0x44, 0x44);
constexpr uint32_t RangedOrange  = rgb(0xF9, 0x73, 0x16);
constexpr uint32_t CasterPurple  = rgb(0xA8, 0x55, 0xF7);
constexpr uint32_t NeutralGray   = rgb(0x9C, 0xA3, 0xAF);

/// Same color at a different opacity. Alpha is absolute, not multiplied in.
[[nodiscard]] constexpr uint32_t with_alpha(uint32_t color, float a) {
    const auto alpha = static_cast<uint32_t>((a < 0.0f ? 0.0f : (a > 1.0f ? 1.0f : a)) * 255.0f);
    return (color & 0x00FFFFFFu) | (alpha << 24);
}

} // namespace colors

/// Layout constants in unscaled (96 DPI) pixels. Read them through m() so every
/// call site picks up the DPI scale the same way.
namespace metrics {
constexpr float Gutter      = 12.0f;  // between siblings in a row or grid
constexpr float PagePad     = 18.0f;  // page edge to content
constexpr float CardPad     = 14.0f;  // card edge to content
constexpr float CardRadius  = 8.0f;
constexpr float ChipSize    = 28.0f;  // icon chip square
constexpr float ChipSizeLg  = 36.0f;
constexpr float RowHeight   = 30.0f;  // one settings row's control
constexpr float ButtonH     = 30.0f;
constexpr float ButtonSm    = 110.0f;
constexpr float ButtonMd    = 150.0f;
constexpr float ButtonLg    = 200.0f;
constexpr float ControlW    = 186.0f; // right-hand control column in a settings row
constexpr float StatTileH   = 92.0f;
constexpr float SidebarW    = 220.0f;
constexpr float NavItemH    = 38.0f;
constexpr float GridMinCol  = 430.0f; // a settings grid drops to one column below this
} // namespace metrics

/// Applies sleek Slate Dark styling to Dear ImGui context
void apply_slate_theme();

/// Sets the DPI scale factor (1.0 == 96 DPI) used to size fonts and hardcoded layout constants.
/// Must be called once at startup, before any UI is rendered.
void set_ui_scale(float scale);

/// Returns the DPI scale factor set via set_ui_scale(), or 1.0 if never set.
[[nodiscard]] float ui_scale();

/// Scales an unscaled pixel constant (a metrics:: value, or a one-off) to the current DPI.
[[nodiscard]] inline float m(float px) { return px * ui_scale(); }

/// Sets the bold ImFont* used for headers and branding text. Called once at startup.
void set_bold_font(ImFont* font);

/// Returns the bold ImFont* set via set_bold_font(), or nullptr if never set.
[[nodiscard]] ImFont* bold_font();

/// Returns IM_COL32 formatted color for FFXIV Job
[[nodiscard]] uint32_t get_job_color_u32(game::Job job, float alpha = 1.0f);

/// Returns IM_COL32 formatted color for FFXIV Role
[[nodiscard]] uint32_t get_role_color_u32(game::Role role, float alpha = 1.0f);

// Formatting helpers
[[nodiscard]] std::string format_dps(double dps);
[[nodiscard]] std::string format_damage(uint64_t damage);
[[nodiscard]] std::string format_percentage(double pct);
[[nodiscard]] std::string format_duration(uint64_t seconds);

} // namespace hub::app::ui
