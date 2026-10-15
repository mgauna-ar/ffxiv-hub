#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace hub::iq {

/// The client's jitter amplitude (0x14025f870) is
/// `(multiplier - 0.001) * s^2 + 0.001`, with
/// `s = applied_height / target_height * rezo_scale`: the height frames render at
/// over the top of the range, times the 3D resolution slider. Under DLSS the game's
/// jitter shrinks as its controller lowers the height.
/// Returns s^2, or nullopt while the heights are unknown.
[[nodiscard]] inline std::optional<float> jitter_scale_squared(uint16_t applied_height, uint16_t target_height,
                                                               float rezo_scale) noexcept {
    if (applied_height == 0 || target_height == 0 || !(rezo_scale > 0.0f)) return std::nullopt;
    const float s = static_cast<float>(applied_height) / static_cast<float>(target_height) * rezo_scale;
    return s * s;
}

/// The screen's height: the top of the range over its share of the screen
/// (RenderScaleState::target_ratio). Nullopt while either is unknown.
[[nodiscard]] inline std::optional<uint16_t> screen_height(uint16_t target_height, float target_ratio) noexcept {
    if (target_height == 0 || !(target_ratio > 0.0f) || target_ratio > 1.0f) return std::nullopt;
    const long height = std::lround(static_cast<double>(target_height) / static_cast<double>(target_ratio));
    if (height > UINT16_MAX) return std::nullopt;
    return static_cast<uint16_t>(height);
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
[[nodiscard]] inline std::optional<uint16_t> pinned_height(uint16_t screen_height, uint16_t floor_height,
                                                           uint16_t target_height, float render_scale) noexcept {
    if (screen_height == 0 || floor_height == 0 || target_height < floor_height ||
        target_height > screen_height) {
        return std::nullopt;
    }
    const long wanted = std::lround(static_cast<double>(screen_height) * static_cast<double>(render_scale));
    return static_cast<uint16_t>(std::clamp<long>(wanted, floor_height, target_height));
}

/// The texture mip bias, in mip levels, that matches rendering `render_height` lines
/// for a `screen_height` screen: log2 of the one over the other, negative below
/// native. Nullopt while either is unknown.
[[nodiscard]] inline std::optional<float> mip_bias_for(uint16_t render_height, uint16_t screen_height) noexcept {
    if (render_height == 0 || screen_height == 0) return std::nullopt;
    return static_cast<float>(std::log2(static_cast<double>(render_height) / static_cast<double>(screen_height)));
}

} // namespace hub::iq
