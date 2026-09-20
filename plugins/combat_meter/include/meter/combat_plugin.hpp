#pragma once

#include "hub/plugin_api.hpp"
#include "meter/encounter_engine.hpp"
#include "common/ipc/ring_buffer.hpp"
#include <memory>

namespace hub::meter {

class CombatPlugin : public IPlugin, public IConfigurable, public IHookConsumer {
public:
    CombatPlugin();
    ~CombatPlugin() override = default;

    // IPlugin
    PluginId id() const noexcept override { return PluginId::CombatMeter; }
    const char* name() const noexcept override { return "Combat Meter"; }
    const char* version() const noexcept override { return "1.0.0"; }

    bool initialize() override;
    void update(double delta_seconds) override;
    void shutdown() override;

    // IConfigurable
    void serialize_config(config::JsonValue& out) const override;
    void deserialize_config(const config::JsonValue& in) override;
    void render_settings_ui() override;

    // IHookConsumer
    void on_receive_action_effect(
        uint32_t source_entity_id,
        const void* source_character,
        const void* effect_header,
        const void* effect_data,
        const uint64_t* targets
    ) override;

    [[nodiscard]] EncounterEngine& engine() noexcept { return m_engine; }
    [[nodiscard]] const EncounterEngine& engine() const noexcept { return m_engine; }

    [[nodiscard]] CombatConfig& config() noexcept { return m_config; }
    [[nodiscard]] const CombatConfig& config() const noexcept { return m_config; }

    /// Sets the outbound packet sink used to stream combat data to the desktop app.
    void set_ring_buffer(ipc::PacketRingBuffer* ring_buffer) noexcept { m_ring_buffer = ring_buffer; }

private:
    EncounterEngine m_engine;
    CombatConfig m_config;
    bool m_initialized{false};
    ipc::PacketRingBuffer* m_ring_buffer{nullptr};
    uint32_t m_sequence{0};
};

} // namespace hub::meter
