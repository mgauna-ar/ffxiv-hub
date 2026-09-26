#include "common/ui/overlay_config.hpp"

#include "common/config/key_table.hpp"

namespace hub::ui {

namespace {

using config::field;

/// The keys every overlay persists, alongside its plugin's own.
constexpr config::ConfigKey<OverlayConfig> OVERLAY_KEYS[] = {
    field<OverlayConfig, &OverlayConfig::visible>("overlay_visible"),
    field<OverlayConfig, &OverlayConfig::locked>("overlay_locked"),
    field<OverlayConfig, &OverlayConfig::click_through>("overlay_click_through"),
    field<OverlayConfig, &OverlayConfig::opacity>("overlay_opacity"),
    field<OverlayConfig, &OverlayConfig::scale>("overlay_scale"),
    field<OverlayConfig, &OverlayConfig::x>("overlay_x"),
    field<OverlayConfig, &OverlayConfig::y>("overlay_y"),
    field<OverlayConfig, &OverlayConfig::width>("overlay_width"),
    field<OverlayConfig, &OverlayConfig::height>("overlay_height"),
    field<OverlayConfig, &OverlayConfig::hide_conditions>("overlay_hide_conditions"),
    field<OverlayConfig, &OverlayConfig::hide_after_combat_s>("overlay_hide_after_combat_seconds"),
};

} // namespace

void serialize_overlay(const OverlayConfig& cfg, config::JsonValue& out) {
    config::write_keys<OverlayConfig>(OVERLAY_KEYS, cfg, out);
}

void store_overlay_geometry(config::JsonValue& section,
                            float x, float y, float width, float height) {
    section["overlay_x"] = config::JsonValue(static_cast<double>(x));
    section["overlay_y"] = config::JsonValue(static_cast<double>(y));
    section["overlay_width"] = config::JsonValue(static_cast<double>(width));
    section["overlay_height"] = config::JsonValue(static_cast<double>(height));
}

OverlayConfig deserialize_overlay(const config::JsonValue& in, const OverlayConfig& defaults) {
    OverlayConfig cfg = defaults;
    config::read_keys<OverlayConfig>(OVERLAY_KEYS, in, cfg);
    return cfg;
}

bool conditions_hide(uint32_t hide_conditions, uint32_t state_flags) noexcept {
    if (hide_conditions == 0) return false;

    const bool in_combat = has_flag(state_flags, GameStateFlag::InCombat);
    if (has_condition(hide_conditions, HideCondition::OutOfCombat) && !in_combat) return true;
    if (has_condition(hide_conditions, HideCondition::InCombat) && in_combat) return true;

    // Everything below comes from the Conditions array alone, so an unresolved
    // source means "don't know" rather than "not happening".
    if (!has_flag(state_flags, GameStateFlag::Valid)) return false;

    if (has_condition(hide_conditions, HideCondition::InCutscene) &&
        has_flag(state_flags, GameStateFlag::InCutscene)) return true;
    if (has_condition(hide_conditions, HideCondition::MenuOpen) &&
        has_flag(state_flags, GameStateFlag::Occupied)) return true;
    if (has_condition(hide_conditions, HideCondition::Loading) &&
        has_flag(state_flags, GameStateFlag::Loading)) return true;
    if (has_condition(hide_conditions, HideCondition::InPvP) &&
        has_flag(state_flags, GameStateFlag::InPvP)) return true;

    const bool in_duty = has_flag(state_flags, GameStateFlag::InDuty);
    if (has_condition(hide_conditions, HideCondition::InDuty) && in_duty) return true;
    if (has_condition(hide_conditions, HideCondition::OutsideDuty) && !in_duty) return true;

    return false;
}

} // namespace hub::ui
