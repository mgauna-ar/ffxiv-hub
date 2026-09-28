#pragma once

#include "hub/game_definitions.hpp"
#include "common/ipc/protocol.hpp"
#include "common/ipc/ring_buffer.hpp"
#include "meter/vitals.hpp"
#include <atomic>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <span>
#include <string>
#include <chrono>
#include <mutex>

namespace hub::meter {
    class CombatantRegistry;
}

namespace hub::payload {

namespace detail {

/// Copies a StatusManager's occupied slots into `out`. False when the layout check
/// fails: a slot count other than 30 or 60, or, with `expected_owner` set, an owner
/// that is neither null nor that object. Without detail (the party list's copy)
/// timers and sources are left at 0.
bool extract_status_list(const game::StatusManagerObject* manager, const void* expected_owner,
                         bool with_detail, meter::ActorVitals& out) noexcept;

/// Copies the occupied slots meter::is_attribution_status keeps, with timers and
/// sources, into `out` and sets `count`. False on the same layout check as
/// extract_status_list.
bool extract_attribution_statuses(const game::StatusManagerObject* manager, const void* expected_owner,
                                  ipc::CombatStatusEntry* out, size_t capacity, size_t& count) noexcept;

/// True when the local player id holds NO_ENTITY_ID, the client's value while no
/// character is in the world. Any other value, or no address, is not evidence.
[[nodiscard]] bool reads_as_lobby(const uint32_t* local_player_id) noexcept;

} // namespace detail

/**
 * @brief SEH-protected memory scanner for in-game Character objects and PartyList.
 *
 * Resolves player names, jobs, health points, and pet-owner relationships directly
 * from FFXIV's internal memory structures.
 */
class ObjectReader {
public:
    using RingBuffer = ipc::PacketRingBuffer;

    explicit ObjectReader(RingBuffer* ring_buffer = nullptr);
    ~ObjectReader() = default;

    /// Scans once, before hooks are installed; the detour thread reads the result
    /// unlocked. Never retried: a missed signature stays missed until the next patch.
    bool initialize();
    bool read_character(uint32_t entity_id, ipc::ActorInfoPacket& out_packet);
    void inspect_and_sync_actor(uint32_t entity_id, meter::CombatantRegistry* registry = nullptr);
    /// True while the object table read for `entity_id` is fresh enough that
    /// inspect_and_sync_actor() would return without reading it again. Takes only
    /// the cache's own lock, so a caller can check it before the engine's.
    [[nodiscard]] bool recently_read(uint32_t entity_id);
    void inspect_and_sync_actor_direct(const void* character_ptr, meter::CombatantRegistry* registry = nullptr);
    void sync_party(meter::CombatantRegistry* registry = nullptr);

    /// HP and status lists for one vitals pass: each party member (the local player
    /// when solo), then `enemy_ids`. Fills `out` from the front and returns how many
    /// entries it filled. Allocates nothing. Orchestration thread only.
    size_t read_vitals(std::span<const uint32_t> enemy_ids, std::span<meter::ActorVitals> out);

    /// False once status reads were switched off because the StatusManager layout
    /// check never passed: the offsets need re-verifying after a patch.
    [[nodiscard]] bool status_reads_enabled() const noexcept {
        return !m_status_reads_disabled.load(std::memory_order_relaxed);
    }

    /// The raid buffs and hit guarantees an actor holds right now, for attributing a
    /// hit: read through `character` when the hook handed one over, else found by id.
    /// Game main thread only, where statuses are written. Nullopt when the actor
    /// cannot be read or status reads are off; never counts toward the layout check.
    [[nodiscard]] std::optional<size_t> read_attribution_statuses(
        uint32_t entity_id, const void* character, std::span<ipc::CombatStatusEntry> out);

    /// True while no character is in the world: title screen, data center or
    /// character select. False when unsure, so a missed signature shows overlays.
    [[nodiscard]] bool in_lobby() const;

    /// Drops every cached actor, party and territory value so the next read
    /// republishes them. The caches exist to keep the same packet off the wire
    /// twice, which also means a listener that reconnects mid-session would
    /// otherwise never be told any name it missed.
    void invalidate_cache();

    /// Territory the party is in as of the last sync_party(); 0 when unknown,
    /// which includes solo play (the party list is empty then).
    [[nodiscard]] uint16_t current_territory() const noexcept { return m_last_territory; }

    [[nodiscard]] bool is_initialized() const noexcept { return m_initialized; }

private:
    using FnGetObjectByEntityId = game::CharacterObject*(void*, uint32_t);

    /// Fields of a Character the game handed us a pointer to; SEH-guarded on Windows.
    static bool read_character_object(const void* character_ptr, ipc::ActorInfoPacket& out_packet);

    /// Cache, register and publish a freshly read actor. Returns false when the
    /// cached copy is identical, i.e. nothing was sent.
    bool publish_actor(const ipc::ActorInfoPacket& packet, meter::CombatantRegistry* registry);

    /// Counts a status read against the layout check. Until one read has passed,
    /// kStatusLayoutStrikes failures switch status reads off.
    void note_status_layout(bool ok);
    static constexpr uint32_t kStatusLayoutStrikes = 20;

    [[maybe_unused]] FnGetObjectByEntityId* m_fp_get_object_by_id{nullptr};
    [[maybe_unused]] uintptr_t m_game_object_mgr_addr{0};
    [[maybe_unused]] uintptr_t m_group_manager_addr{0};
    /// Static uint32 holding the local player's entity id; 0 when its signature missed.
    [[maybe_unused]] uintptr_t m_local_player_id_addr{0};
    RingBuffer* m_ring_buffer{nullptr};
    bool m_initialized{false};

    struct CachedActor {
        uint32_t owner_id{0};
        uint32_t job_id{0};
        uint32_t max_hp{0};
        std::string name;
        /// Last time the object table was read for this actor. Monsters and NPCs
        /// have job_id 0 forever, so "has a job" cannot be the freshness test.
        std::chrono::steady_clock::time_point last_read{};
        /// HP is left out of the change test so it does not resend every tick, but
        /// a death or a raise has to reach the app's wipe detection.
        bool dead{false};
    };

    [[nodiscard]] static bool is_dead(uint32_t current_hp, uint32_t max_hp) noexcept {
        return max_hp > 0 && current_hp == 0;
    }

    /// How long a cached actor is trusted before the object table is read again.
    static constexpr std::chrono::seconds kActorCacheTtl{5};

    std::mutex m_cache_mutex;
    std::unordered_map<uint32_t, CachedActor> m_actor_cache;
    ipc::PartySyncPacket m_last_party_sync{};
    uint16_t m_last_territory{0};
    /// False until m_last_territory has been announced; 0 is a real value to send.
    bool m_territory_published{false};

    // Status layout check; orchestration thread only, but the main thread reads the switch.
    uint32_t m_status_layout_failures{0};
    bool m_status_layout_confirmed{false};
    std::atomic<bool> m_status_reads_disabled{false};

    /// Status and param pairs already logged for buffs whose strength the applying
    /// action decides. Main thread only.
    std::unordered_set<uint32_t> m_logged_strength_params;
};

} // namespace hub::payload
