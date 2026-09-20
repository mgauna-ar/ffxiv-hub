#include "mitigator/latency_plugin.hpp"
#include "hub/game_definitions.hpp"
#include "common/config/json.hpp"
#include <cstring>

namespace hub::mitigator {

LatencyPlugin::LatencyPlugin() = default;

bool LatencyPlugin::initialize() {
    m_initialized = true;
    return true;
}

void LatencyPlugin::update(double /*delta_seconds*/) {
    // Periodic maintenance if needed (e.g. prune stale requests)
}

void LatencyPlugin::shutdown() {
    m_initialized = false;
    m_mitigator.reset();
}

void LatencyPlugin::serialize_config(config::JsonValue& out) const {
    const auto cfg = m_mitigator.get_config();
    out["enabled"] = config::JsonValue(cfg.enabled);
    out["dry_run"] = config::JsonValue(cfg.dry_run);
    out["target_ping_ms"] = config::JsonValue(cfg.target_ping_ms);
    out["min_animation_lock_ms"] = config::JsonValue(cfg.min_animation_lock_ms);
    out["max_animation_lock_ms"] = config::JsonValue(cfg.max_animation_lock_ms);
    out["rtt_sample_window"] = config::JsonValue(static_cast<uint32_t>(cfg.rtt_sample_window));
    out["safety_margin_ms"] = config::JsonValue(cfg.safety_margin_ms);
}

void LatencyPlugin::deserialize_config(const config::JsonValue& in) {
    if (!in.is_object()) return;
    auto cfg = m_mitigator.get_config();

    if (in.contains("enabled")) cfg.enabled = in["enabled"].as_bool(cfg.enabled);
    if (in.contains("dry_run")) cfg.dry_run = in["dry_run"].as_bool(cfg.dry_run);
    if (in.contains("target_ping_ms")) cfg.target_ping_ms = in["target_ping_ms"].as_double(cfg.target_ping_ms);
    if (in.contains("min_animation_lock_ms")) cfg.min_animation_lock_ms = in["min_animation_lock_ms"].as_double(cfg.min_animation_lock_ms);
    if (in.contains("max_animation_lock_ms")) cfg.max_animation_lock_ms = in["max_animation_lock_ms"].as_double(cfg.max_animation_lock_ms);
    if (in.contains("rtt_sample_window")) cfg.rtt_sample_window = static_cast<size_t>(in["rtt_sample_window"].as_int(static_cast<int>(cfg.rtt_sample_window)));
    if (in.contains("safety_margin_ms")) cfg.safety_margin_ms = in["safety_margin_ms"].as_double(cfg.safety_margin_ms);

    m_mitigator.set_config(cfg);
}

void LatencyPlugin::render_settings_ui() {
    // Rendered in desktop app during Session 5
}

void LatencyPlugin::on_use_action_location(
    void* action_mgr,
    uint32_t /*action_type*/,
    uint32_t action_id,
    uint64_t /*target_id*/,
    const void* /*location*/,
    uint32_t /*extra*/,
    uint64_t result
) {
    // Invariant: If client rejected action (result == 0), no packet was sent to server
    if (result == 0 || action_mgr == nullptr) {
        return;
    }

    const auto* mgr_bytes = reinterpret_cast<const uint8_t*>(action_mgr);
    uint16_t current_sequence = 0;
    std::memcpy(&current_sequence, mgr_bytes + game::offsets::ACTION_MANAGER_CURRENT_SEQUENCE, sizeof(uint16_t));

    bool is_casting = false;
    std::memcpy(&is_casting, mgr_bytes + game::offsets::ACTION_MANAGER_IS_CASTING, sizeof(bool));

    float cast_time = 0.0f;
    if (is_casting) {
        std::memcpy(&cast_time, mgr_bytes + game::offsets::ACTION_MANAGER_CAST_TIME, sizeof(float));
    }

    m_mitigator.record_action_request(
        action_id,
        current_sequence,
        std::chrono::steady_clock::now(),
        is_casting,
        cast_time
    );
}

void LatencyPlugin::on_receive_action_effect(
    uint32_t /*source_entity_id*/,
    const void* /*source_character*/,
    const void* effect_header,
    const void* /*effect_data*/,
    const uint64_t* /*targets*/
) {
    if (!effect_header) return;

    const auto* header = reinterpret_cast<const game::ActionEffectHeader*>(effect_header);
    const double server_lock_ms = static_cast<double>(header->animation_lock) * 1000.0;

    // Only process effects that assign an animation lock
    if (server_lock_ms <= 0.0) return;

    const auto res = m_mitigator.calculate_mitigation(
        header->action_id,
        header->source_sequence,
        server_lock_ms
    );

    // If mitigation was applied and not in dry-run, modify animation lock in ActionManager
    // (Note: in live payload, the pointer to ActionManager is provided via HookManager)
    (void)res;
}

} // namespace hub::mitigator
