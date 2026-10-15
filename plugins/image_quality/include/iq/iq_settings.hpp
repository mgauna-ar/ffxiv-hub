#pragma once

#include "iq/types.hpp"
#include "common/config/json.hpp"

namespace hub::iq {

/// Everything the image_quality section of config.json holds but the master
/// switch. Its keys are declared once, in the table in iq_settings.cpp.
struct ImageQualitySettings {
    /// Under DLSS, stop the game's frame-time controller and render at
    /// dlss_render_scale of the screen's height.
    bool fixed_dlss_resolution{true};
    float dlss_render_scale{constants::DEFAULT_DLSS_RENDER_SCALE};
    /// Under DLSS, make the camera move by the sample the game tells DLSS about,
    /// instead of the game's smaller amount.
    bool override_dlss_jitter{true};
    /// Under DLSS, while the render height is held, add log2 of the held height over
    /// the screen's to the game's texture mip bias, plus dlss_mip_bias_offset.
    bool dlss_mip_bias{true};
    float dlss_mip_bias_offset{0.0f};
};

/// `value` within the DLSS render scale's range; NaN reads as the default.
[[nodiscard]] float clamp_render_scale(float value) noexcept;

/// `value` within the mip bias offset's range, on its step; NaN reads as 0.
[[nodiscard]] float clamp_mip_bias_offset(float value) noexcept;

/// Writes every plugin key (not the master switch) from `settings` into `section`.
void write_settings(const ImageQualitySettings& settings, config::JsonValue& section);

/// Reads the plugin keys present in `section` into `settings`; absent ones keep their value.
void read_settings(const config::JsonValue& section, ImageQualitySettings& settings);

/// Every key the plugin writes, at its default, without the master switch.
[[nodiscard]] config::JsonValue default_settings();

} // namespace hub::iq
