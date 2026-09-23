#pragma once

#include "hub/game_definitions.hpp"
#include "common/ipc/protocol.hpp"
#include "common/ipc/ring_buffer.hpp"
#include <cstdint>
#include <unordered_map>
#include <string>
#include <chrono>
#include <mutex>

namespace hub::meter {
    class CombatantRegistry;
}

namespace hub::payload {

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
    void inspect_and_sync_actor_direct(const void* character_ptr, meter::CombatantRegistry* registry = nullptr);
    void sync_party(meter::CombatantRegistry* registry = nullptr);

    /// Drops every cached actor, party and territory value so the next read
    /// republishes them. The caches exist to keep the same packet off the wire
    /// twice, which also means a listener that reconnects mid-session would
    /// otherwise never be told any name it missed.
    void invalidate_cache();

    /// Territory the party is in as of the last sync_party(); 0 when unknown,
    /// which includes solo play (the party list is empty then).
    [[nodiscard]] uint16_t current_territory() const noexcept { return m_last_territory; }

    void set_ring_buffer(RingBuffer* ring_buffer) noexcept { m_ring_buffer = ring_buffer; }
    [[nodiscard]] bool is_initialized() const noexcept { return m_initialized; }

private:
    using FnGetObjectByEntityId = game::CharacterObject*(void*, uint32_t);

    /// Fields of a Character the game handed us a pointer to; SEH-guarded on Windows.
    static bool read_character_object(const void* character_ptr, ipc::ActorInfoPacket& out_packet);

    /// Cache, register and publish a freshly read actor. Returns false when the
    /// cached copy is identical, i.e. nothing was sent.
    bool publish_actor(const ipc::ActorInfoPacket& packet, meter::CombatantRegistry* registry);

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
};

} // namespace hub::payload
