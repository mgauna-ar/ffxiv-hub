#pragma once

#include "hub/game_definitions.hpp"
#include "common/ipc/protocol.hpp"
#include "common/ipc/ring_buffer.hpp"
#include <cstdint>
#include <unordered_map>
#include <string>
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
    using RingBuffer = ipc::SpscRingBuffer<std::vector<uint8_t>, 4096>;

    explicit ObjectReader(RingBuffer* ring_buffer = nullptr);
    ~ObjectReader() = default;

    bool initialize();
    bool read_character(uint32_t entity_id, ipc::ActorInfoPacket& out_packet);
    void inspect_and_sync_actor(uint32_t entity_id, meter::CombatantRegistry* registry = nullptr);
    void inspect_and_sync_actor_direct(void* character_ptr, meter::CombatantRegistry* registry = nullptr);
    void sync_party(meter::CombatantRegistry* registry = nullptr);

    void set_ring_buffer(RingBuffer* ring_buffer) noexcept { m_ring_buffer = ring_buffer; }
    [[nodiscard]] bool is_initialized() const noexcept { return m_initialized; }

private:
    using FnGetObjectByEntityId = game::CharacterObject*(void*, uint32_t);

    [[maybe_unused]] FnGetObjectByEntityId* m_fp_get_object_by_id{nullptr};
    [[maybe_unused]] uintptr_t m_game_object_mgr_addr{0};
    [[maybe_unused]] uintptr_t m_group_manager_addr{0};
    RingBuffer* m_ring_buffer{nullptr};
    bool m_initialized{false};

    struct CachedActor {
        uint32_t owner_id{0};
        uint32_t job_id{0};
        uint32_t max_hp{0};
        std::string name;
    };

    std::mutex m_cache_mutex;
    [[maybe_unused]] std::unordered_map<uint32_t, CachedActor> m_actor_cache;
    [[maybe_unused]] ipc::PartySyncPacket m_last_party_sync{};
};

} // namespace hub::payload
