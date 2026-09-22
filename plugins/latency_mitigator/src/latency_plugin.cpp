#include "mitigator/latency_plugin.hpp"
#include "mitigator/latency_overlay.hpp"
#include "hub/game_definitions.hpp"
#include "common/config/json.hpp"
#include "common/ipc/protocol.hpp"
#include <cstring>
#include <cmath>
#include <chrono>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace hub::mitigator {

namespace {

// SEH-protected leaf functions for touching ActionManager memory. Kept free of
// C++ objects requiring unwinding, matching this codebase's SafeCallOriginal*/
// SafeRead* convention (src/payload/hook_manager.cpp, object_reader.cpp).
#ifdef _WIN32
float SafeReadAnimationLock(void* mgr) {
    __try {
        if (mgr != nullptr) {
            return *reinterpret_cast<float*>(static_cast<uint8_t*>(mgr) + game::offsets::ACTION_MANAGER_ANIMATION_LOCK);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0.0f;
    }
    return 0.0f;
}

bool SafeWriteAnimationLock(void* mgr, float desired_lock) {
    __try {
        if (mgr != nullptr && std::isfinite(desired_lock) && desired_lock >= 0.0f) {
            *reinterpret_cast<float*>(static_cast<uint8_t*>(mgr) + game::offsets::ACTION_MANAGER_ANIMATION_LOCK) = desired_lock;
            return true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return false;
}
#else
float SafeReadAnimationLock(void* mgr) {
    if (mgr == nullptr) return 0.0f;
    return *reinterpret_cast<float*>(static_cast<uint8_t*>(mgr) + game::offsets::ACTION_MANAGER_ANIMATION_LOCK);
}

bool SafeWriteAnimationLock(void* mgr, float desired_lock) {
    if (mgr == nullptr || !std::isfinite(desired_lock) || desired_lock < 0.0f) return false;
    *reinterpret_cast<float*>(static_cast<uint8_t*>(mgr) + game::offsets::ACTION_MANAGER_ANIMATION_LOCK) = desired_lock;
    return true;
}
#endif

} // namespace

LatencyPlugin::LatencyPlugin() = default;

bool LatencyPlugin::initialize() {
    m_initialized = true;
    return true;
}

void LatencyPlugin::update(double /*delta_seconds*/) {
    // Periodic maintenance if needed (e.g. prune stale requests)
}

void LatencyPlugin::set_plugin_enabled(bool enabled) noexcept {
    m_plugin_enabled.store(enabled);
    // A disabled plugin must not leave its HUD painted over the game.
    if (!enabled && m_overlay) {
        m_overlay->set_visible(false);
    }
}

void LatencyPlugin::shutdown() {
    m_initialized = false;
    m_mitigator.reset();
}

void LatencyPlugin::serialize_config(config::JsonValue& out) const {
    const auto cfg = m_mitigator.get_config();
    out["plugin_enabled"] = config::JsonValue(m_plugin_enabled.load());
    out["enabled"] = config::JsonValue(cfg.enabled);
    out["dry_run"] = config::JsonValue(cfg.dry_run);
    out["target_ping_ms"] = config::JsonValue(cfg.target_ping_ms);
    out["min_animation_lock_ms"] = config::JsonValue(cfg.min_animation_lock_ms);
    out["max_animation_lock_ms"] = config::JsonValue(cfg.max_animation_lock_ms);
    out["rtt_sample_window"] = config::JsonValue(static_cast<uint32_t>(cfg.rtt_sample_window));
    out["safety_margin_ms"] = config::JsonValue(cfg.safety_margin_ms);
    out["spike_multiplier"] = config::JsonValue(cfg.spike_multiplier);

    // The overlay owns anything the player changes live in-game, so read the
    // live state back out whenever there is one.
    ui::serialize_overlay(m_overlay ? m_overlay->capture_config() : m_overlay_config, out);
    if (m_overlay) {
        out["overlay_mode"] = config::JsonValue(static_cast<int>(m_overlay->display_mode()));
    }
}

void LatencyPlugin::deserialize_config(const config::JsonValue& in) {
    if (!in.is_object()) return;
    auto cfg = m_mitigator.get_config();

    if (in.contains("plugin_enabled")) {
        m_plugin_enabled.store(in["plugin_enabled"].as_bool(m_plugin_enabled.load()));
    }
    if (in.contains("enabled")) cfg.enabled = in["enabled"].as_bool(cfg.enabled);
    if (in.contains("dry_run")) cfg.dry_run = in["dry_run"].as_bool(cfg.dry_run);
    if (in.contains("target_ping_ms")) cfg.target_ping_ms = in["target_ping_ms"].as_double(cfg.target_ping_ms);
    if (in.contains("min_animation_lock_ms")) cfg.min_animation_lock_ms = in["min_animation_lock_ms"].as_double(cfg.min_animation_lock_ms);
    if (in.contains("max_animation_lock_ms")) cfg.max_animation_lock_ms = in["max_animation_lock_ms"].as_double(cfg.max_animation_lock_ms);
    if (in.contains("rtt_sample_window")) cfg.rtt_sample_window = static_cast<size_t>(in["rtt_sample_window"].as_int(static_cast<int>(cfg.rtt_sample_window)));
    if (in.contains("safety_margin_ms")) cfg.safety_margin_ms = in["safety_margin_ms"].as_double(cfg.safety_margin_ms);
    if (in.contains("spike_multiplier")) cfg.spike_multiplier = in["spike_multiplier"].as_double(cfg.spike_multiplier);

    m_mitigator.set_config(cfg);

    m_overlay_config = ui::deserialize_overlay(in, m_overlay_config);

    if (m_overlay) {
        m_overlay->apply_config(m_overlay_config);
        // apply_config restores the persisted visibility, which a disabled plugin
        // must not get back.
        if (!m_plugin_enabled.load()) m_overlay->set_visible(false);
        if (in.contains("overlay_mode")) {
            m_overlay->set_display_mode(static_cast<OverlayDisplayMode>(in["overlay_mode"].as_int(static_cast<int>(m_overlay->display_mode()))));
        }
    }
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
    if (!m_plugin_enabled.load() || result == 0 || action_mgr == nullptr) {
        return;
    }

    m_action_manager.store(action_mgr);

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

void LatencyPlugin::on_action_manager_resolved(void* action_manager) {
    if (action_manager != nullptr) {
        m_action_manager.store(action_manager);
    }
}

void LatencyPlugin::on_pre_receive_action_effect() {
    if (!m_plugin_enabled.load()) return;
    m_pre_lock_snapshot.store(SafeReadAnimationLock(m_action_manager.load()));
}

void LatencyPlugin::on_receive_action_effect(
    uint32_t /*source_entity_id*/,
    const void* /*source_character*/,
    const void* effect_header,
    const void* /*effect_data*/,
    const uint64_t* /*targets*/
) {
    if (!m_plugin_enabled.load() || !effect_header) return;

    // ReceiveActionEffect fires for every actor in the zone, so only mitigate
    // when this call actually changed our own animation_lock.
    void* mgr = m_action_manager.load();
    const float old_lock = m_pre_lock_snapshot.load();
    const float new_lock = SafeReadAnimationLock(mgr);
    const bool lock_changed = std::abs(new_lock - old_lock) > 0.0001f;

    if (!lock_changed || new_lock <= game::definitions::MIN_ACTION_EFFECT_LOCK_SECONDS || !std::isfinite(new_lock)) {
        return;
    }

    const auto* header = reinterpret_cast<const game::ActionEffectHeader*>(effect_header);
    const double original_lock_ms = static_cast<double>(new_lock) * 1000.0;

    const auto res = m_mitigator.calculate_mitigation(
        header->action_id,
        header->source_sequence,
        original_lock_ms
    );

    // Skip the write-back/telemetry while the desktop app is disconnected —
    // nothing is listening, and we shouldn't be silently mutating game memory
    // with no operator able to observe or disable it.
    if (!m_connected.load()) {
        return;
    }

    if (res.applied) {
        const float adjusted_seconds = static_cast<float>(res.adjusted_lock_ms / 1000.0);
        SafeWriteAnimationLock(mgr, adjusted_seconds);
    }

    if (m_ring_buffer) {
        ipc::MitigatorTelemetryPayload payload{};
        payload.action_id = res.action_id;
        payload.sequence = res.sequence;
        payload.original_lock_ms = static_cast<float>(res.original_lock_ms);
        payload.adjusted_lock_ms = static_cast<float>(res.adjusted_lock_ms);
        payload.delay_reduced_ms = static_cast<float>(res.delay_reduced_ms);
        payload.measured_rtt_ms = static_cast<float>(res.measured_rtt_ms);
        payload.smoothed_rtt_ms = static_cast<float>(res.smoothed_rtt_ms);
        payload.jitter_ms = static_cast<float>(m_mitigator.get_rtt_tracker().get_jitter_ms());
        payload.clamped_floor = res.clamped_by_floor ? 1 : 0;
        payload.dry_run = m_mitigator.get_config().dry_run ? 1 : 0;
        payload.applied = res.applied ? 1 : 0;
        payload.cast_active = res.cast_active ? 1 : 0;
        payload.spike_filtered = res.spike_filtered ? 1 : 0;
        payload.cold_start_guard = res.cold_start_guard ? 1 : 0;
        payload.timestamp_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()
            ).count()
        );

        auto bytes = ipc::serialize_typed_packet(PluginId::LatencyMitigator, MessageType::MitigatorTelemetry, 0, payload);
        m_ring_buffer->push(bytes);
    }
}

} // namespace hub::mitigator
