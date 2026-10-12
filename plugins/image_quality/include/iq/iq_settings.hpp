#pragma once

#include "iq/types.hpp"
#include "common/config/json.hpp"

namespace hub::iq {

/// Everything the image_quality section of config.json holds but the master
/// switch. Its keys are declared once, in the table in iq_settings.cpp.
struct ImageQualitySettings {
    /// Under DLSS, stop the game's frame-time controller and render at the
    /// configured scale.
    bool fixed_dlss_resolution{true};
    /// Under DLSS, replace the game's jitter multiplier with dlss_jitter_multiplier.
    bool override_dlss_jitter{true};
    float dlss_jitter_multiplier{constants::DEFAULT_JITTER_MULTIPLIER};
};

/// `value` within the slider's range; NaN reads as the default.
[[nodiscard]] float clamp_jitter_multiplier(float value) noexcept;

/// Writes every plugin key (not the master switch) from `settings` into `section`.
void write_settings(const ImageQualitySettings& settings, config::JsonValue& section);

/// Reads the plugin keys present in `section` into `settings`; absent ones keep their value.
void read_settings(const config::JsonValue& section, ImageQualitySettings& settings);

/// Every key the plugin writes, at its default, without the master switch.
[[nodiscard]] config::JsonValue default_settings();

} // namespace hub::iq
