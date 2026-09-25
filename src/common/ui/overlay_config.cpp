#include "common/ui/overlay_config.hpp"

#include "common/config/json.hpp"

namespace hub::ui {

void serialize_overlay(const OverlayConfig& cfg, config::JsonValue& out) {
    out["overlay_visible"] = config::JsonValue(cfg.visible);
    out["overlay_locked"] = config::JsonValue(cfg.locked);
    out["overlay_click_through"] = config::JsonValue(cfg.click_through);
    out["overlay_opacity"] = config::JsonValue(static_cast<double>(cfg.opacity));
    out["overlay_scale"] = config::JsonValue(static_cast<double>(cfg.scale));
    out["overlay_x"] = config::JsonValue(static_cast<double>(cfg.x));
    out["overlay_y"] = config::JsonValue(static_cast<double>(cfg.y));
    out["overlay_width"] = config::JsonValue(static_cast<double>(cfg.width));
    out["overlay_height"] = config::JsonValue(static_cast<double>(cfg.height));
    out["overlay_hide_conditions"] = config::JsonValue(cfg.hide_conditions);
    out["overlay_hide_after_combat_seconds"] = config::JsonValue(static_cast<double>(cfg.hide_after_combat_s));
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
    if (!in.is_object()) return cfg;

    const auto read_bool = [&](const char* key, bool& field) {
        if (in.contains(key)) field = in[key].as_bool(field);
    };
    const auto read_float = [&](const char* key, float& field) {
        if (in.contains(key)) field = static_cast<float>(in[key].as_double(field));
    };

    read_bool("overlay_visible", cfg.visible);
    read_bool("overlay_locked", cfg.locked);
    read_bool("overlay_click_through", cfg.click_through);
    read_float("overlay_opacity", cfg.opacity);
    read_float("overlay_scale", cfg.scale);
    read_float("overlay_x", cfg.x);
    read_float("overlay_y", cfg.y);
    read_float("overlay_width", cfg.width);
    read_float("overlay_height", cfg.height);
    if (in.contains("overlay_hide_conditions")) {
        cfg.hide_conditions = static_cast<uint32_t>(
            in["overlay_hide_conditions"].as_int(static_cast<int>(cfg.hide_conditions)));
    }
    read_float("overlay_hide_after_combat_seconds", cfg.hide_after_combat_s);
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
