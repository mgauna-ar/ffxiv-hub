#include "mitigator/latency_plugin.hpp"
#include "mitigator/latency_overlay.hpp"
#include "mitigator/latency_settings.hpp"
#include "hub/game_definitions.hpp"
#include "common/config/json.hpp"
#include "common/ipc/protocol.hpp"
#include "common/os/safe_memory.hpp"
#include <cmath>
#include <chrono>
#include <utility>

namespace hub::mitigator {

namespace {

/// What UseActionLocation left in the ActionManager.
struct UseActionState {
    uint16_t sequence{0};
    uint8_t is_casting{0};
    float cast_time{0.0f};
    float elapsed_cast_time{0.0f};
};

// ActionManager memory is the game's, so every access goes through the SEH-guarded
// hub::os::safe_copy; each returns false (or 0) for a null or faulting manager.
uintptr_t field(void* mgr, uintptr_t offset) {
    return mgr != nullptr ? reinterpret_cast<uintptr_t>(mgr) + offset : 0;
}

bool SafeReadUseActionState(void* mgr, UseActionState& out) {
    return hub::os::safe_read(field(mgr, game::offsets::ACTION_MANAGER_CURRENT_SEQUENCE), out.sequence) &&
           hub::os::safe_read(field(mgr, game::offsets::ACTION_MANAGER_IS_CASTING), out.is_casting) &&
           hub::os::safe_read(field(mgr, game::offsets::ACTION_MANAGER_CAST_TIME), out.cast_time) &&
           hub::os::safe_read(field(mgr, game::offsets::ACTION_MANAGER_ELAPSED_CAST_TIME), out.elapsed_cast_time);
}

float SafeReadAnimationLock(void* mgr) {
    float lock = 0.0f;
    return hub::os::safe_read(field(mgr, game::offsets::ACTION_MANAGER_ANIMATION_LOCK), lock) ? lock : 0.0f;
}

bool SafeWriteAnimationLock(void* mgr, float desired_lock) {
    if (!std::isfinite(desired_lock) || desired_lock < 0.0f) return false;
    const uintptr_t addr = field(mgr, game::offsets::ACTION_MANAGER_ANIMATION_LOCK);
    return hub::os::safe_copy(reinterpret_cast<void*>(addr), &desired_lock, sizeof(desired_lock));
}

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

LatencySettings LatencyPlugin::live_settings() const {
    // The overlay owns anything the player changes live in-game, so read the
    // live state back out whenever there is one.
    LatencySettings settings;
    settings.mitigation = m_mitigator.get_config();
    settings.overlay = m_overlay ? m_overlay->capture_config() : m_overlay_config;
    settings.overlay_mode = m_overlay ? m_overlay->display_mode() : m_overlay_mode;
    return settings;
}

void LatencyPlugin::serialize_config(config::JsonValue& out) const {
    out = config::JsonValue(config::JsonValue::ObjectType{});
    out[plugins::MASTER_SWITCH_KEY] = config::JsonValue(m_plugin_enabled.load());
    write_settings(live_settings(), out);
}

void LatencyPlugin::deserialize_config(const config::JsonValue& in) {
    if (!in.is_object()) return;

    if (in.contains(plugins::MASTER_SWITCH_KEY)) {
        m_plugin_enabled.store(in[plugins::MASTER_SWITCH_KEY].as_bool(m_plugin_enabled.load()));
    }

    // A key absent from `in` keeps the value in effect now.
    LatencySettings next = live_settings();
    read_settings(in, next);

    m_mitigator.set_config(next.mitigation);
    m_overlay_config = next.overlay;
    m_overlay_mode = next.overlay_mode;

    // plugin_enabled may have just changed.
    refresh_overlay_suppression();

    if (m_overlay) {
        m_overlay->apply_config(m_overlay_config);
        m_overlay->set_display_mode(m_overlay_mode);
    }
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
        m_ring_buffer->push(std::move(bytes));
    }
}

} // namespace hub::mitigator
