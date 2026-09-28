#pragma once

#include "hub/plugin_api.hpp"
#include "hub/plugin_registry.hpp"
#include "meter/buff_attribution.hpp"
#include "meter/encounter_engine.hpp"
#include "meter/vitals.hpp"
#include "common/ipc/ring_buffer.hpp"
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace hub::meter {

class CombatOverlay;

class CombatPlugin : public IPlugin, public IConfigurable, public IHookConsumer {
public:
    CombatPlugin();
    ~CombatPlugin() override = default;

    // IPlugin
    PluginId id() const noexcept override { return plugins::COMBAT_METER.id; }
    const char* name() const noexcept override { return plugins::COMBAT_METER.name; }
    const char* version() const noexcept override { return plugins::COMBAT_METER.version; }

    bool initialize() override;
    void update(double delta_seconds) override;

    // IConfigurable
    void serialize_config(config::JsonValue& out) const override;
    void deserialize_config(const config::JsonValue& in) override;

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
    /// or raise first, then an enemy's HP and the status list if they changed, all
    /// applied locally and published. With track_vitals off nothing is read, and
    /// every list published so far is cleared once.
    void on_vitals(std::span<const ActorVitals> actors, uint64_t now_us);

    /// Whether the payload should read vitals at all this pass.
    [[nodiscard]] bool vitals_enabled() const noexcept;
    void set_vitals_tracking(bool enabled) noexcept;

    /// Which damage rate the overlay shows and ranks by, kept for the next save too.
    void set_dps_metric(DpsMetric metric) noexcept;
    [[nodiscard]] DpsMetric dps_metric() const noexcept {
        return dps_metric_from(m_dps_metric.load(std::memory_order_relaxed));
    }

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

    /// Same job for a source the hook did hand a character pointer for, given the
    /// actor the plugin already read from it. The pointer path is the common one,
    /// and registering it locally is not enough: the desktop app runs its own
    /// engine and only ever learns a name from an ActorInfo packet, so that read
    /// has to reach the wire too.
    void set_actor_object_resolver(std::function<void(const ipc::ActorInfoPacket&)> resolver) {
        m_actor_object_resolver = std::move(resolver);
    }

    /// Reads an actor's current and max HP from the game's object table, so each
    /// heal can be split into effective healing and overheal before either engine
    /// sees it. Returns false when the actor cannot be read.
    using HpResolver = std::function<bool(uint32_t entity_id, uint32_t& current_hp, uint32_t& max_hp)>;
    void set_hp_resolver(HpResolver resolver) { m_hp_resolver = std::move(resolver); }

    /// Reads the raid buffs and hit guarantees an actor holds right now, from
    /// `character` when the hook handed one over and from the object table otherwise.
    /// Fills `out` from the front and returns how many, or nullopt when the actor
    /// cannot be read. Called on the game's main thread, where statuses change, so the
    /// list is exactly what the server had applied before the hit being attributed.
    using StatusReader = std::function<std::optional<size_t>(
        uint32_t entity_id, const void* character, std::span<ipc::CombatStatusEntry> out)>;
    void set_status_reader(StatusReader reader) { m_status_reader = std::move(reader); }

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

    /// The settings in effect now, from the overlay and the atomics where they live.
    [[nodiscard]] CombatConfig live_config() const;

    /// Attribution statuses read for one actor.
    struct StatusSnapshot {
        static constexpr size_t kCapacity = 32;
        std::array<ipc::CombatStatusEntry, kCapacity> entries{};
        size_t count{0};
        bool read{false};

        [[nodiscard]] std::span<const ipc::CombatStatusEntry> view() const noexcept {
            return {entries.data(), count};
        }
    };
    [[nodiscard]] StatusSnapshot read_statuses(uint32_t entity_id, const void* character) const;

    /// Credits for one damage hit from `owner`; empty when it earns none. A clean hit
    /// also feeds the owner's rate estimate.
    [[nodiscard]] ipc::CombatBuffCredits attribute(
        const CombatantRegistry& registry, EntityId owner, ActionId action_id, uint32_t damage,
        uint16_t hit_flags, const StatusSnapshot& on_source, const StatusSnapshot& on_target, bool is_tick);

    /// The attacker's buffs when it applied a status to an enemy, for that status's
    /// ticks: a DoT keeps the buffs it was applied under.
    struct DotKey {
        uint32_t target{0};
        uint32_t source{0};
        uint16_t status{0};
        bool operator==(const DotKey&) const = default;
    };
    struct DotKeyHash {
        size_t operator()(const DotKey& key) const noexcept {
            const uint64_t ids = (static_cast<uint64_t>(key.target) << 32) | key.source;
            return std::hash<uint64_t>{}(ids ^ (static_cast<uint64_t>(key.status) * 0x9E3779B97F4A7C15ull));
        }
    };
    struct DotSnapshot {
        uint64_t applied_us{0};
        StatusSnapshot source;
    };
    void remember_dot(const DotKey& key, const StatusSnapshot& source, uint64_t now_us);
    static constexpr size_t kMaxDotSnapshots = 256;
    static constexpr uint64_t kDotSnapshotTtlUs = 120'000'000;

    EncounterEngine m_engine;
    CombatConfig m_config;
    bool m_initialized{false};
    /// m_config.enabled as the other threads read it; set by commands on the orchestration thread.
    std::atomic<bool> m_enabled{true};
    /// True until the payload says otherwise, so a plugin with a sink streams to it.
    std::atomic<bool> m_connected{true};
    ipc::PacketRingBuffer* m_ring_buffer{nullptr};
    std::function<void(uint32_t)> m_actor_resolver;
    std::function<void(const ipc::ActorInfoPacket&)> m_actor_object_resolver;
    HpResolver m_hp_resolver;
    StatusReader m_status_reader;

    // Game main thread only, under the engine lock.
    RateEstimator m_rates;
    BuffStrengths m_strengths;
    std::unordered_map<DotKey, DotSnapshot, DotKeyHash> m_dot_snapshots;
    CombatOverlay* m_overlay{nullptr};
    GameStateProvider* m_game_state{nullptr};
    uint32_t m_sequence{0};

    /// Read and set on the orchestration thread; atomic for the detour thread.
    std::atomic<bool> m_track_vitals{true};
    /// Set by a command and saved by the autosave, both on the orchestration thread.
    std::atomic<uint32_t> m_dps_metric{0};
    // Orchestration thread only, under the engine lock.
    VitalsTracker m_vitals;
    std::vector<EntityId> m_vitals_seen;
    uint32_t m_vitals_sequence{0};
    uint64_t m_vitals_pulls_seen{0};
};

} // namespace hub::meter
