#pragma once

#include "hub/plugin_api.hpp"
#include "mitigator/animation_lock.hpp"
#include <memory>

namespace hub::mitigator {

class LatencyPlugin : public IPlugin, public IConfigurable, public IHookConsumer {
public:
    LatencyPlugin();
    ~LatencyPlugin() override = default;

    // IPlugin
    PluginId id() const noexcept override { return PluginId::LatencyMitigator; }
    const char* name() const noexcept override { return "Latency Mitigator"; }
    const char* version() const noexcept override { return "1.0.0"; }

    bool initialize() override;
    void update(double delta_seconds) override;
    void shutdown() override;

    // IConfigurable
    void serialize_config(config::JsonValue& out) const override;
    void deserialize_config(const config::JsonValue& in) override;
    void render_settings_ui() override;

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

    void on_receive_action_effect(
        uint32_t source_entity_id,
        const void* source_character,
        const void* effect_header,
        const void* effect_data,
        const uint64_t* targets
    ) override;

    [[nodiscard]] AnimationLockMitigator& mitigator() noexcept { return m_mitigator; }
    [[nodiscard]] const AnimationLockMitigator& mitigator() const noexcept { return m_mitigator; }

private:
    AnimationLockMitigator m_mitigator;
    bool m_initialized{false};
};

} // namespace hub::mitigator
