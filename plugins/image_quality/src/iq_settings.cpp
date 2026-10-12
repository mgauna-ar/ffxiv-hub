#include "iq/iq_settings.hpp"
#include "common/config/key_table.hpp"
#include <algorithm>
#include <cmath>

namespace hub::iq {

namespace {

using config::field;
using S = ImageQualitySettings;

/// Every key the plugin persists besides the master switch.
constexpr config::ConfigKey<S> KEYS[] = {
    field<S, &S::fixed_dlss_resolution>("fixed_dlss_resolution"),
    field<S, &S::override_dlss_jitter>("override_dlss_jitter"),
    config::ConfigKey<S>{
        "dlss_jitter_multiplier",
        [](const S& s) { return config::JsonValue(static_cast<double>(s.dlss_jitter_multiplier)); },
        [](const config::JsonValue& v, S& s) {
            if (v.is_number()) s.dlss_jitter_multiplier = clamp_jitter_multiplier(v.as_float(s.dlss_jitter_multiplier));
        },
    },
};

} // namespace

float clamp_jitter_multiplier(float value) noexcept {
    if (std::isnan(value)) return constants::DEFAULT_JITTER_MULTIPLIER;
    return std::clamp(value, constants::MIN_JITTER_MULTIPLIER, constants::MAX_JITTER_MULTIPLIER);
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
