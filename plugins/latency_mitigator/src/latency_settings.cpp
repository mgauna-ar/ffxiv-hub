#include "mitigator/latency_settings.hpp"
#include "common/config/key_table.hpp"

namespace hub::mitigator {

namespace {

using config::field;
using S = LatencySettings;
using M = MitigationConfig;

/// Every key the plugin persists besides the shared overlay keys
/// (ui::serialize_overlay) and the master switch.
constexpr config::ConfigKey<S> KEYS[] = {
    // Stops the write-back only; the master switch is plugin_enabled.
    field<S, &S::mitigation, &M::enabled>("enabled"),
    field<S, &S::mitigation, &M::dry_run>("dry_run"),
    field<S, &S::mitigation, &M::target_ping_ms>("target_ping_ms"),
    field<S, &S::mitigation, &M::min_animation_lock_ms>("min_animation_lock_ms"),
    field<S, &S::mitigation, &M::max_animation_lock_ms>("max_animation_lock_ms"),
    config::ConfigKey<S>{
        "rtt_sample_window",
        [](const S& s) { return config::JsonValue(static_cast<double>(s.mitigation.rtt_sample_window)); },
        [](const config::JsonValue& v, S& s) {
            // Anything below 1 keeps the current window.
            const int window = v.as_int(0);
            if (window >= 1) s.mitigation.rtt_sample_window = static_cast<size_t>(window);
        },
    },
    field<S, &S::mitigation, &M::safety_margin_ms>("safety_margin_ms"),
    field<S, &S::mitigation, &M::spike_multiplier>("spike_multiplier"),
    field<S, &S::overlay_mode>("overlay_mode"),
};

} // namespace

void write_settings(const LatencySettings& settings, config::JsonValue& section) {
    config::write_keys<S>(KEYS, settings, section);
    ui::serialize_overlay(settings.overlay, section);
}

void read_settings(const config::JsonValue& section, LatencySettings& settings) {
    config::read_keys<S>(KEYS, section, settings);
    settings.overlay = ui::deserialize_overlay(section, settings.overlay);
}

config::JsonValue default_settings() {
    config::JsonValue section{config::JsonValue::ObjectType{}};
    write_settings(LatencySettings{}, section);
    return section;
}

} // namespace hub::mitigator
