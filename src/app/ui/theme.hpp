#pragma once

#include "hub/types.hpp"
#include "meter/types.hpp"
#include <cstdint>
#include <string>

namespace hub::app::ui {

namespace colors {
    // Slate Dark UI Palette
    constexpr uint32_t BackgroundDark  = 0xFF140E0B; // #0B0E14
    constexpr uint32_t CardBackground   = 0xFF231915; // #151923
    constexpr uint32_t CardHovered      = 0xFF2C221C; // #1C222C
    constexpr uint32_t Border           = 0xFF45322A; // #2A3245
    constexpr uint32_t TextWhite        = 0xFFF6F4F3; // #F3F4F6
    constexpr uint32_t TextMuted        = 0xFFAFB79C; // #9CB7AF / #9CA3AF
    constexpr uint32_t AccentGold       = 0xFF4DD3FC; // #FCD34D

    // FFXIV Role Palette
    constexpr uint32_t TankBlue         = 0xFFF6823B; // #3B82F6
    constexpr uint32_t HealerGreen      = 0xFF81B910; // #10B981
    constexpr uint32_t MeleeRed         = 0xFF4444EF; // #EF4444
    constexpr uint32_t RangedOrange     = 0xFF1673F9; // #F97316
    constexpr uint32_t CasterPurple     = 0xFFF755A8; // #A855F7
    constexpr uint32_t NeutralGray      = 0xFFAFA39C; // #9CA3AF
}

/// Applies sleek Slate Dark styling to Dear ImGui context
void apply_slate_theme();

/// Returns IM_COL32 formatted color for FFXIV Job
[[nodiscard]] uint32_t get_job_color_u32(meter::Job job, float alpha = 1.0f);

/// Returns IM_COL32 formatted color for FFXIV Role
[[nodiscard]] uint32_t get_role_color_u32(meter::Role role, float alpha = 1.0f);

// Formatting helpers
[[nodiscard]] std::string format_dps(double dps);
[[nodiscard]] std::string format_damage(uint64_t damage);
[[nodiscard]] std::string format_percentage(double pct);
[[nodiscard]] std::string format_duration(uint64_t seconds);

} // namespace hub::app::ui
