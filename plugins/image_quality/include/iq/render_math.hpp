#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace hub::iq {

/// The client's jitter amplitude (0x14025f870) is
/// `(multiplier - 0.001) * s^2 + 0.001`, with
/// `s = display_height / target_height * rezo_scale`. Under DLSS the target is the
/// top of DLSS's own range, the screen height, so `s` is the 3D resolution slider.
/// Returns s^2, or nullopt while the heights are unknown.
[[nodiscard]] inline std::optional<float> jitter_scale_squared(uint16_t display_height, uint16_t target_height,
                                                               float rezo_scale) noexcept {
    if (display_height == 0 || target_height == 0 || !(rezo_scale > 0.0f)) return std::nullopt;
    const float s = static_cast<float>(display_height) / static_cast<float>(target_height) * rezo_scale;
    return s * s;
}

/// The amplitude the client jitters by for `multiplier`.
[[nodiscard]] inline float effective_jitter(float multiplier, float scale_squared) noexcept {
    return (multiplier - 0.001f) * scale_squared + 0.001f;
}

/// The multiplier that makes the client jitter by `amount`, kept within
/// [`min_multiplier`, `max_multiplier`].
[[nodiscard]] inline float multiplier_for(float amount, float scale_squared, float min_multiplier,
                                          float max_multiplier) noexcept {
    const float multiplier = (amount - 0.001f) / scale_squared + 0.001f;
    return std::clamp(multiplier, min_multiplier, max_multiplier);
}

/// The render height for `render_scale` of the screen, within the range the
/// resolution controller moves in. Nullopt while that range is unknown.
[[nodiscard]] inline std::optional<uint16_t> pinned_height(uint16_t display_height, uint16_t floor_height,
                                                           uint16_t target_height, float render_scale) noexcept {
    if (display_height == 0 || floor_height == 0 || target_height < floor_height ||
        target_height > display_height) {
        return std::nullopt;
    }
    const long wanted = std::lround(static_cast<double>(display_height) * static_cast<double>(render_scale));
    return static_cast<uint16_t>(std::clamp<long>(wanted, floor_height, target_height));
}

} // namespace hub::iq
