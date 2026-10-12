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
    /// Under DLSS, make the camera jitter by dlss_jitter_amount instead of the
    /// game's amount.
    bool override_dlss_jitter{true};
    float dlss_jitter_amount{constants::DEFAULT_JITTER_AMOUNT};
};

/// `value` within the jitter slider's range; NaN reads as the default.
[[nodiscard]] float clamp_jitter_amount(float value) noexcept;

/// `value` within the DLSS render scale's range; NaN reads as the default.
[[nodiscard]] float clamp_render_scale(float value) noexcept;

/// Writes every plugin key (not the master switch) from `settings` into `section`.
void write_settings(const ImageQualitySettings& settings, config::JsonValue& section);

/// Reads the plugin keys present in `section` into `settings`; absent ones keep their value.
void read_settings(const config::JsonValue& section, ImageQualitySettings& settings);

/// Every key the plugin writes, at its default, without the master switch.
[[nodiscard]] config::JsonValue default_settings();

} // namespace hub::iq
