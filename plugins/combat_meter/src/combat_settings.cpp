#include "meter/combat_settings.hpp"
#include "common/config/key_table.hpp"

namespace hub::meter {

namespace {

using config::field;

/// Every key the plugin persists besides the shared overlay keys
/// (ui::serialize_overlay) and the master switch.
constexpr config::ConfigKey<CombatConfig> KEYS[] = {
    field<CombatConfig, &CombatConfig::party_only>("party_only"),
    field<CombatConfig, &CombatConfig::show_bars>("show_bars"),
    field<CombatConfig, &CombatConfig::hide_inactive>("hide_inactive"),
    field<CombatConfig, &CombatConfig::refresh_interval_ms>("refresh_interval_ms"),
    field<CombatConfig, &CombatConfig::show_col_share>("show_col_share"),
    field<CombatConfig, &CombatConfig::show_col_crit>("show_col_crit"),
    field<CombatConfig, &CombatConfig::show_col_dh>("show_col_dh"),
    field<CombatConfig, &CombatConfig::show_col_cdh>("show_col_cdh"),
    field<CombatConfig, &CombatConfig::overlay_metric>("overlay_metric"),
    field<CombatConfig, &CombatConfig::dps_metric>("dps_metric"),
    field<CombatConfig, &CombatConfig::track_vitals>("track_vitals"),
};

} // namespace

void write_settings(const CombatConfig& cfg, config::JsonValue& section) {
    config::write_keys<CombatConfig>(KEYS, cfg, section);
    ui::serialize_overlay(cfg.overlay, section);
}

void read_settings(const config::JsonValue& section, CombatConfig& cfg) {
    config::read_keys<CombatConfig>(KEYS, section, cfg);
    cfg.overlay = ui::deserialize_overlay(section, cfg.overlay);
}

config::JsonValue default_settings() {
    config::JsonValue section{config::JsonValue::ObjectType{}};
    write_settings(CombatConfig{}, section);
    return section;
}

config::JsonValue desktop_default_settings() {
    config::JsonValue section{config::JsonValue::ObjectType{}};
    section[DESKTOP_DPS_METRIC_KEY] = config::JsonValue(static_cast<int>(DpsMetric::Dps));
    section[PULL_HISTORY_LIMIT_KEY] = config::JsonValue(static_cast<int>(constants::DEFAULT_HISTORY_CAPACITY));
    return section;
}

} // namespace hub::meter
