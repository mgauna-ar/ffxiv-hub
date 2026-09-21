#pragma once

#include "hub/plugin_api.hpp"
#include "meter/encounter_engine.hpp"
#include "common/ipc/ring_buffer.hpp"
#include <functional>
#include <memory>

namespace hub::meter {

class CombatOverlay;

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

    void on_status_tick(
        uint32_t target_entity_id,
        uint32_t source_entity_id,
        uint16_t status_id,
        uint32_t damage_or_heal,
        bool is_heal
    ) override;

    [[nodiscard]] EncounterEngine& engine() noexcept { return m_engine; }
    [[nodiscard]] const EncounterEngine& engine() const noexcept { return m_engine; }

    [[nodiscard]] CombatConfig& config() noexcept { return m_config; }
    [[nodiscard]] const CombatConfig& config() const noexcept { return m_config; }

    /// Sets the outbound packet sink used to stream combat data to the desktop app.
    void set_ring_buffer(ipc::PacketRingBuffer* ring_buffer) noexcept { m_ring_buffer = ring_buffer; }

    /// Supplies a lookup that fills in an actor's name/job/HP from the game's
    /// object table. Action packets only carry a character pointer for the
    /// source, and not always, so targets are otherwise unidentifiable.
    void set_actor_resolver(std::function<void(uint32_t)> resolver) { m_actor_resolver = std::move(resolver); }

    /// Non-owning pointer to the in-game overlay this plugin drives via config load/commands.
    void set_overlay(CombatOverlay* overlay) noexcept { m_overlay = overlay; }

    /// The meter derives combat from packets, which survives a signature break
    /// after a game patch, so it feeds that bit back for every overlay to use.
    void set_game_state(GameStateProvider* provider) noexcept { m_game_state = provider; }
    [[nodiscard]] CombatOverlay* overlay() const noexcept { return m_overlay; }

private:
    EncounterEngine m_engine;
    CombatConfig m_config;
    bool m_initialized{false};
    ipc::PacketRingBuffer* m_ring_buffer{nullptr};
    std::function<void(uint32_t)> m_actor_resolver;
    CombatOverlay* m_overlay{nullptr};
    GameStateProvider* m_game_state{nullptr};
    uint32_t m_sequence{0};
};

} // namespace hub::meter
