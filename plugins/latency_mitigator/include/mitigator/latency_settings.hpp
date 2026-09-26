#pragma once

#include "mitigator/types.hpp"
#include "common/config/json.hpp"

namespace hub::mitigator {

/// Everything the latency_mitigator section of config.json holds but the master
/// switch. Its keys are declared once, in the table in latency_settings.cpp.
struct LatencySettings {
    MitigationConfig mitigation{};
    ui::OverlayConfig overlay{default_overlay_config()};
    OverlayDisplayMode overlay_mode{OverlayDisplayMode::CompactInline};
};

/// Writes every plugin key (not the master switch) from `settings` into `section`.
void write_settings(const LatencySettings& settings, config::JsonValue& section);

/// Reads the plugin keys present in `section` into `settings`; absent ones keep their value.
void read_settings(const config::JsonValue& section, LatencySettings& settings);

/// Every key the plugin writes, at its default, without the master switch.
[[nodiscard]] config::JsonValue default_settings();

} // namespace hub::mitigator
