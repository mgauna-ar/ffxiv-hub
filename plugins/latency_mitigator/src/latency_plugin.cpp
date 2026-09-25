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

/// What UseActionLocation left in the ActionManager, read in one guarded pass.
struct UseActionState {
    uint16_t sequence{0};
    uint8_t is_casting{0};
    float cast_time{0.0f};
    float elapsed_cast_time{0.0f};
};

void extract_use_action_state(const uint8_t* mgr, UseActionState& out) {
    std::memcpy(&out.sequence, mgr + game::offsets::ACTION_MANAGER_CURRENT_SEQUENCE, sizeof(out.sequence));
    std::memcpy(&out.is_casting, mgr + game::offsets::ACTION_MANAGER_IS_CASTING, sizeof(out.is_casting));
    std::memcpy(&out.cast_time, mgr + game::offsets::ACTION_MANAGER_CAST_TIME, sizeof(out.cast_time));
    std::memcpy(&out.elapsed_cast_time, mgr + game::offsets::ACTION_MANAGER_ELAPSED_CAST_TIME, sizeof(out.elapsed_cast_time));
}

// SEH-protected leaf functions for touching ActionManager memory. Kept free of
// C++ objects requiring unwinding, matching this codebase's SafeCallOriginal*/
// SafeRead* convention (src/payload/hook_manager.cpp, object_reader.cpp).
#ifdef _WIN32
bool SafeReadUseActionState(void* mgr, UseActionState& out) {
    __try {
        if (mgr != nullptr) {
            extract_use_action_state(static_cast<const uint8_t*>(mgr), out);
            return true;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return false;
}

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
bool SafeReadUseActionState(void* mgr, UseActionState& out) {
    if (mgr == nullptr) return false;
    extract_use_action_state(static_cast<const uint8_t*>(mgr), out);
    return true;
}

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
    refresh_overlay_suppression();
}

void LatencyPlugin::set_connected(bool connected) noexcept {
    m_connected.store(connected);
    refresh_overlay_suppression();
}

void LatencyPlugin::set_overlay(LatencyOverlay* overlay) noexcept {
    m_overlay = overlay;
    refresh_overlay_suppression();
}

void LatencyPlugin::refresh_overlay_suppression() noexcept {
    // Suppressed rather than hidden: overlay_visible is the player's choice and
    // the autosave would persist a hide.
    if (m_overlay) {
        m_overlay->set_suppressed(!m_plugin_enabled.load() || !m_connected.load());
    }
}

void LatencyPlugin::shutdown() {
    m_initialized = false;
    m_mitigator.reset();
}

void LatencyPlugin::serialize_config(config::JsonValue& out) const {
    const auto cfg = m_mitigator.get_config();
    out = config::JsonValue(config::JsonValue::ObjectType{});
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
    if (in.contains("rtt_sample_window")) {
        // A negative value would wrap to a huge size_t; anything below 1 keeps the current window.
        const int window = in["rtt_sample_window"].as_int(static_cast<int>(cfg.rtt_sample_window));
        if (window >= 1) cfg.rtt_sample_window = static_cast<size_t>(window);
    }
    if (in.contains("safety_margin_ms")) cfg.safety_margin_ms = in["safety_margin_ms"].as_double(cfg.safety_margin_ms);
    if (in.contains("spike_multiplier")) cfg.spike_multiplier = in["spike_multiplier"].as_double(cfg.spike_multiplier);

    m_mitigator.set_config(cfg);

    m_overlay_config = ui::deserialize_overlay(in, m_overlay_config);

    // plugin_enabled may have just changed.
    refresh_overlay_suppression();

    if (m_overlay) {
        m_overlay->apply_config(m_overlay_config);
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

    UseActionState state{};
    if (!SafeReadUseActionState(action_mgr, state)) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const bool is_casting = state.is_casting != 0;

    // Second caster-tax guard, independent of the is_cast flag on the request. The
    // cast in progress may have started before this press, so track what is left.
    // A send with no cast in progress ends any tracked one: a cancelled cast sends
    // no effect, and would otherwise hold off mitigation until its time ran out.
    if (is_casting) {
        m_mitigator.record_cast_begin(action_id, state.cast_time - state.elapsed_cast_time, now);
    } else {
        m_mitigator.record_cast_interrupt(now);
    }

    m_mitigator.record_action_request(
        action_id,
        state.sequence,
        now,
        is_casting,
        is_casting ? state.cast_time : 0.0f
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

    if (res.spike_filtered && m_overlay) {
        m_overlay->notify_spike_filtered();
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
