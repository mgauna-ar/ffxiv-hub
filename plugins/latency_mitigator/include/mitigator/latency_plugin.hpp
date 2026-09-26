#pragma once

#include "hub/plugin_api.hpp"
#include "hub/plugin_registry.hpp"
#include "mitigator/animation_lock.hpp"
#include "common/ipc/ring_buffer.hpp"
#include <atomic>
#include <memory>

namespace hub::mitigator {

class LatencyOverlay;

class LatencyPlugin : public IPlugin, public IConfigurable, public IHookConsumer {
public:
    LatencyPlugin();
    ~LatencyPlugin() override = default;

    // IPlugin
    PluginId id() const noexcept override { return plugins::LATENCY_MITIGATOR.id; }
    const char* name() const noexcept override { return plugins::LATENCY_MITIGATOR.name; }
    const char* version() const noexcept override { return plugins::LATENCY_MITIGATOR.version; }

    bool initialize() override;
    void update(double delta_seconds) override;

    // IConfigurable
    void serialize_config(config::JsonValue& out) const override;
    void deserialize_config(const config::JsonValue& in) override;

    // IHookConsumer
    void on_use_action_location(
        void* action_mgr,
        uint32_t action_type,
        uint32_t action_id,
        uint64_t target_id,
        const void* location,
        uint32_t extra,
        uint64_t result
    ) override;

    void on_pre_receive_action_effect() override;

    void on_action_manager_resolved(void* action_manager) override;

    void on_receive_action_effect(
        uint32_t source_entity_id,
        const void* source_character,
        const void* effect_header,
        const void* effect_data,
        const uint64_t* targets
    ) override;

    [[nodiscard]] AnimationLockMitigator& mitigator() noexcept { return m_mitigator; }
    [[nodiscard]] const AnimationLockMitigator& mitigator() const noexcept { return m_mitigator; }

    /// Sets the outbound packet sink used to stream telemetry to the desktop app.
    void set_ring_buffer(ipc::PacketRingBuffer* ring_buffer) noexcept { m_ring_buffer = ring_buffer; }

    /// Non-owning pointer to the in-game HUD this plugin drives via config load/commands.
    void set_overlay(LatencyOverlay* overlay) noexcept;
    [[nodiscard]] LatencyOverlay* overlay() const noexcept { return m_overlay; }

    /// Gates mitigation write-back/telemetry and the HUD while the desktop app is
    /// disconnected, without touching the user's dry_run or visibility preference.
    void set_connected(bool connected) noexcept;

    /// Master switch for the whole plugin. Off means no hook dispatch, no
    /// telemetry and no HUD, distinct from the mitigation switch on the
    /// mitigator, which only stops the memory write-back.
    void set_plugin_enabled(bool enabled) noexcept;
    [[nodiscard]] bool is_plugin_enabled() const noexcept { return m_plugin_enabled.load(); }

private:
    /// The HUD shows only while the plugin is on and the app is listening.
    void refresh_overlay_suppression() noexcept;

    AnimationLockMitigator m_mitigator;
    bool m_initialized{false};
    ipc::PacketRingBuffer* m_ring_buffer{nullptr};
    LatencyOverlay* m_overlay{nullptr};
    /// Last-loaded overlay state, used when no overlay instance is attached.
    ui::OverlayConfig m_overlay_config{default_overlay_config()};
    std::atomic<void*> m_action_manager{nullptr};
    std::atomic<float> m_pre_lock_snapshot{0.0f};
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_plugin_enabled{true};
};

} // namespace hub::mitigator
