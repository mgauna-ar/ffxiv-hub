#pragma once

#include "meter/types.hpp"
#include "common/config/json.hpp"

namespace hub::meter {

// The combat_meter section of config.json. Its keys are declared once, in the
// table in combat_settings.cpp, which both the plugin and the app read from.

/// Key the desktop's Damage tab and Timeline read their damage rate from.
inline constexpr const char* DESKTOP_DPS_METRIC_KEY = "desktop_dps_metric";
/// Key the desktop's pull archive size is read from.
inline constexpr const char* PULL_HISTORY_LIMIT_KEY = "pull_history_limit";
/// The master switch this plugin shipped with, still read for old files.
inline constexpr const char* LEGACY_ENABLED_KEY = "enabled";

/// Writes every plugin key (not the master switch) from `cfg` into `section`.
void write_settings(const CombatConfig& cfg, config::JsonValue& section);

/// Reads the plugin keys present in `section` into `cfg`; absent ones keep their value.
void read_settings(const config::JsonValue& section, CombatConfig& cfg);

/// Every key the payload writes, at its default. The master switch is left out
/// so the legacy "enabled" key keeps its meaning on load.
[[nodiscard]] config::JsonValue default_settings();

/// The section's keys only the desktop app reads and writes, at their defaults.
/// The payload never writes them.
[[nodiscard]] config::JsonValue desktop_default_settings();

} // namespace hub::meter
