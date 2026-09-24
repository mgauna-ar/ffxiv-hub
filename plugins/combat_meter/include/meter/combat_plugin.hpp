#pragma once

#include "hub/plugin_api.hpp"
#include "meter/encounter_engine.hpp"
#include "meter/vitals.hpp"
#include "common/ipc/ring_buffer.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <span>
#include <vector>

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

    /// Master switch for the whole plugin. Off means no hook dispatch, no
    /// packets to the desktop app, and no overlay.
    void set_enabled(bool enabled) noexcept;
    [[nodiscard]] bool is_enabled() const noexcept { return m_enabled.load(std::memory_order_relaxed); }

    /// Stops streaming and hides the overlay while the desktop app is disconnected:
    /// a backlog queued for nobody would reach the next app at once and be booked as
    /// a pull milliseconds long. The in-game engine keeps counting.
    void set_connected(bool connected) noexcept;

    /// One vitals pass, from the payload's orchestration thread: each actor's death
    /// or raise first, then its status list if it changed, both applied locally and
    /// published. With track_vitals off nothing is read, and every list published
    /// so far is cleared once.
    void on_vitals(std::span<const ActorVitals> actors, uint64_t now_us);

    /// Whether the payload should read vitals at all this pass.
    [[nodiscard]] bool vitals_enabled() const noexcept;
    void set_vitals_tracking(bool enabled) noexcept;

    /// A listener that just connected has none of the lists already published.
    void invalidate_published_vitals();

    [[nodiscard]] CombatConfig& config() noexcept { return m_config; }
    [[nodiscard]] const CombatConfig& config() const noexcept { return m_config; }

    /// Sets the outbound packet sink used to stream combat data to the desktop app.
    void set_ring_buffer(ipc::PacketRingBuffer* ring_buffer) noexcept { m_ring_buffer = ring_buffer; }

    /// Supplies a lookup that fills in an actor's name/job/HP from the game's
    /// object table. Action packets only carry a character pointer for the
    /// source, and not always, so targets are otherwise unidentifiable.
    /// All three resolvers must be set before the hooks are installed: the
    /// detour thread reads them without a lock.
    void set_actor_resolver(std::function<void(uint32_t)> resolver) { m_actor_resolver = std::move(resolver); }

    /// Same job for a source the hook did hand a character pointer for. The
    /// pointer path is the common one, and registering it locally is not enough:
    /// the desktop app runs its own engine and only ever learns a name from an
    /// ActorInfo packet, so that read has to reach the wire too.
    void set_actor_object_resolver(std::function<void(const void*)> resolver) {
        m_actor_object_resolver = std::move(resolver);
    }

    /// Reads an actor's current and max HP from the game's object table, so each
    /// heal can be split into effective healing and overheal before either engine
    /// sees it. Returns false when the actor cannot be read.
    using HpResolver = std::function<bool(uint32_t entity_id, uint32_t& current_hp, uint32_t& max_hp)>;
    void set_hp_resolver(HpResolver resolver) { m_hp_resolver = std::move(resolver); }

    /// Non-owning pointer to the in-game overlay this plugin drives via config load/commands.
    void set_overlay(CombatOverlay* overlay) noexcept;

    /// The meter derives combat from packets, which survives a signature break
    /// after a game patch, so it feeds that bit back for every overlay to use.
    void set_game_state(GameStateProvider* provider) noexcept { m_game_state = provider; }
    [[nodiscard]] CombatOverlay* overlay() const noexcept { return m_overlay; }

private:
    [[nodiscard]] bool streaming() const noexcept {
        return m_ring_buffer != nullptr && m_connected.load(std::memory_order_relaxed);
    }

    /// The overlay shows only while the plugin is on and the app is listening.
    void refresh_overlay_suppression() noexcept;

    EncounterEngine m_engine;
    CombatConfig m_config;
    bool m_initialized{false};
    /// m_config.enabled as the other threads read it; set from the pipe reader thread.
    std::atomic<bool> m_enabled{true};
    /// True until the payload says otherwise, so a plugin with a sink streams to it.
    std::atomic<bool> m_connected{true};
    ipc::PacketRingBuffer* m_ring_buffer{nullptr};
    std::function<void(uint32_t)> m_actor_resolver;
    std::function<void(const void*)> m_actor_object_resolver;
    HpResolver m_hp_resolver;
    CombatOverlay* m_overlay{nullptr};
    GameStateProvider* m_game_state{nullptr};
    uint32_t m_sequence{0};

    /// Read off the orchestration thread; set from the pipe reader thread.
    std::atomic<bool> m_track_vitals{true};
    // Orchestration thread only, under the engine lock.
    VitalsTracker m_vitals;
    std::vector<EntityId> m_vitals_seen;
    uint32_t m_vitals_sequence{0};
    uint64_t m_vitals_pulls_seen{0};
};

} // namespace hub::meter
