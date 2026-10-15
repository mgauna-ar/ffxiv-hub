#include "iq/iq_settings.hpp"
#include "common/config/key_table.hpp"
#include <algorithm>
#include <cmath>

namespace hub::iq {

namespace {

using config::field;
using S = ImageQualitySettings;

/// A float key read through `clamp`.
template <float S::*Member, float (*Clamp)(float) noexcept>
constexpr config::ConfigKey<S> clamped_float(const char* name) noexcept {
    return config::ConfigKey<S>{
        name,
        [](const S& s) { return config::JsonValue(static_cast<double>(s.*Member)); },
        [](const config::JsonValue& v, S& s) {
            if (v.is_number()) s.*Member = Clamp(v.as_float(s.*Member));
        },
    };
}

/// Every key the plugin persists besides the master switch.
constexpr config::ConfigKey<S> KEYS[] = {
    field<S, &S::fixed_dlss_resolution>("fixed_dlss_resolution"),
    clamped_float<&S::dlss_render_scale, clamp_render_scale>("dlss_render_scale"),
    field<S, &S::override_dlss_jitter>("override_dlss_jitter"),
    field<S, &S::dlss_mip_bias>("dlss_mip_bias"),
    clamped_float<&S::dlss_mip_bias_offset, clamp_mip_bias_offset>("dlss_mip_bias_offset"),
};

} // namespace

float clamp_render_scale(float value) noexcept {
    if (std::isnan(value)) return constants::DEFAULT_DLSS_RENDER_SCALE;
    return std::clamp(value, constants::MIN_DLSS_RENDER_SCALE, constants::MAX_DLSS_RENDER_SCALE);
}

float clamp_mip_bias_offset(float value) noexcept {
    if (std::isnan(value)) return 0.0f;
    const float clamped = std::clamp(value, constants::MIN_MIP_BIAS_OFFSET, constants::MAX_MIP_BIAS_OFFSET);
    return std::round(clamped / constants::MIP_BIAS_OFFSET_STEP) * constants::MIP_BIAS_OFFSET_STEP;
}

void write_settings(const ImageQualitySettings& settings, config::JsonValue& section) {
    config::write_keys<S>(KEYS, settings, section);
}

void read_settings(const config::JsonValue& section, ImageQualitySettings& settings) {
    config::read_keys<S>(KEYS, section, settings);
}

config::JsonValue default_settings() {
    config::JsonValue section{config::JsonValue::ObjectType{}};
    write_settings(ImageQualitySettings{}, section);
    return section;
}

} // namespace hub::iq
