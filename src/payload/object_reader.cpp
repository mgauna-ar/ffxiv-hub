#include "payload/object_reader.hpp"
#include "meter/combatant_registry.hpp"
#include "common/sigscan.hpp"
#include "common/pe_scanner.hpp"
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace hub::payload {

namespace {

using FnGetObjectByEntityId = game::CharacterObject*(void*, uint32_t);

static bool SafeReadCharacterFromObject(
    game::CharacterObject* obj,
    ipc::ActorInfoPacket& out
) {
    __try {
        if (obj == nullptr) return false;

        out.entity_id = obj->entity_id;
        if (out.entity_id == 0 || out.entity_id == 0xE0000000) return false;

        out.owner_id = (obj->owner_id != 0xE0000000) ? obj->owner_id : 0;
        out.job_id = obj->class_job;
        out.max_hp = obj->max_hp;
        out.current_hp = obj->current_hp;

        if (obj->object_kind == 5 || (out.owner_id != 0)) {
            out.actor_type = static_cast<uint8_t>(meter::ActorType::Pet);
        } else if (obj->object_kind == 1) {
            out.actor_type = static_cast<uint8_t>(meter::ActorType::Player);
        } else {
            out.actor_type = static_cast<uint8_t>(meter::ActorType::Monster);
        }

        std::memcpy(out.name, obj->name, sizeof(out.name) - 1);
        out.name[sizeof(out.name) - 1] = '\0';
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static bool SafeReadCharacter(
    FnGetObjectByEntityId* fn,
    uintptr_t game_obj_mgr_addr,
    uint32_t entity_id,
    ipc::ActorInfoPacket& out
) {
    __try {
        if (entity_id == 0 || entity_id == 0xE0000000 || game_obj_mgr_addr == 0) {
            return false;
        }

        void* object_arrays = reinterpret_cast<void*>(game_obj_mgr_addr + 0x20);
        game::CharacterObject* obj = nullptr;

        if (fn != nullptr) {
            obj = fn(object_arrays, entity_id);
        }

        if (obj != nullptr) {
            return SafeReadCharacterFromObject(obj, out);
        }
        return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct ExtractedPartyMember {
    uint32_t entity_id{0};
    uint32_t current_hp{0};
    uint32_t max_hp{0};
    uint8_t job_id{0};
    char name[64]{0};
};

struct ExtractedParty {
    uint8_t count{0};
    ExtractedPartyMember members[game::definitions::MAX_PARTY_MEMBERS];
};

static bool SafeReadParty(
    uintptr_t group_mgr_addr,
    ipc::PartySyncPacket& out,
    ExtractedParty& out_members
) {
    __try {
        if (group_mgr_addr == 0) return false;

        // GroupManager is the static instance itself, not a pointer to one.
        const uintptr_t main_group = group_mgr_addr + game::offsets::GROUP_MAIN_GROUP;

        uint8_t count = *reinterpret_cast<const uint8_t*>(main_group + game::offsets::GROUP_MEMBER_COUNT);
        if (count > game::definitions::MAX_PARTY_MEMBERS) {
            count = static_cast<uint8_t>(game::definitions::MAX_PARTY_MEMBERS);
        }

        out.party_count = count;
        out_members.count = count;

        for (uint8_t i = 0; i < count; ++i) {
            const auto* member = reinterpret_cast<const game::PartyMemberObject*>(
                main_group + (i * game::offsets::PARTY_MEMBER_SIZE)
            );
            out.entity_ids[i] = member->entity_id;
            out.job_ids[i] = member->class_job;

            auto& extracted = out_members.members[i];
            extracted.entity_id = member->entity_id;
            extracted.current_hp = member->current_hp;
            extracted.max_hp = member->max_hp;
            extracted.job_id = member->class_job;
            std::memcpy(extracted.name, member->name, sizeof(extracted.name) - 1);
            extracted.name[sizeof(extracted.name) - 1] = '\0';
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

ObjectReader::ObjectReader(RingBuffer* ring_buffer)
    : m_ring_buffer(ring_buffer) {}

bool ObjectReader::initialize() {
    if (m_initialized) return true;

    HMODULE h_game = GetModuleHandleW(nullptr);
    if (!h_game) return false;

    // Scan for GameObjectManager
    uintptr_t mgr_ins = common::pe::scan_module_section(h_game, ".text", game::signatures::GAME_OBJECT_MANAGER_INSTANCE);
    if (mgr_ins) {
        m_game_object_mgr_addr = hub::memory::resolve_rip_relative(mgr_ins, 3, 7);
    }

    // Scan for GetObjectByEntityId
    uintptr_t get_fn = common::pe::scan_module_section(h_game, ".text", game::signatures::GET_OBJECT_BY_ENTITY_ID);
    if (!get_fn) {
        get_fn = common::pe::scan_module_section(h_game, ".text", game::signatures::GET_OBJECT_BY_ENTITY_ID_FALLBACK);
    }
    if (get_fn) {
        m_fp_get_object_by_id = reinterpret_cast<FnGetObjectByEntityId*>(get_fn);
    }

    // Scan for GroupManager
    uintptr_t group_ins = common::pe::scan_module_section(h_game, ".text", game::signatures::GROUP_MANAGER_INSTANCE);
    if (group_ins) {
        m_group_manager_addr = hub::memory::resolve_rip_relative(group_ins, 5, 9);
    }

    m_initialized = (m_game_object_mgr_addr != 0 || m_group_manager_addr != 0);
    return m_initialized;
}

bool ObjectReader::read_character(uint32_t entity_id, ipc::ActorInfoPacket& out_packet) {
    if (!m_initialized && !initialize()) return false;
    return SafeReadCharacter(m_fp_get_object_by_id, m_game_object_mgr_addr, entity_id, out_packet);
}

void ObjectReader::inspect_and_sync_actor(uint32_t entity_id, meter::CombatantRegistry* registry) {
    if (entity_id == 0 || entity_id == 0xE0000000) {
        return;
    }

    // This runs per decoded effect, so skip the SEH + object-table lookup once
    // the actor is fully identified.
    {
        std::lock_guard<std::mutex> lock(m_cache_mutex);
        auto it = m_actor_cache.find(entity_id);
        if (it != m_actor_cache.end() && it->second.job_id != 0 && !it->second.name.empty()) {
            return;
        }
    }

    ipc::ActorInfoPacket packet{};
    if (read_character(entity_id, packet)) {
        {
            std::lock_guard<std::mutex> lock(m_cache_mutex);
            m_actor_cache[entity_id] = CachedActor{packet.owner_id, packet.job_id, packet.max_hp, packet.name};
        }
        if (registry) {
            registry->register_actor(
                packet.entity_id,
                packet.name,
                static_cast<meter::Job>(packet.job_id),
                packet.owner_id,
                static_cast<meter::ActorType>(packet.actor_type),
                packet.max_hp,
                packet.current_hp
            );
        }

        if (m_ring_buffer) {
            std::vector<uint8_t> bytes = ipc::serialize_typed_packet(
                PluginId::CombatMeter, MessageType::CombatActorInfo, 0, packet
            );
            m_ring_buffer->push(bytes);
        }
    }
}

void ObjectReader::inspect_and_sync_actor_direct(void* character_ptr, meter::CombatantRegistry* registry) {
    if (!character_ptr) return;
    ipc::ActorInfoPacket packet{};
    if (SafeReadCharacterFromObject(reinterpret_cast<game::CharacterObject*>(character_ptr), packet)) {
        if (registry) {
            registry->register_actor(
                packet.entity_id,
                packet.name,
                static_cast<meter::Job>(packet.job_id),
                packet.owner_id,
                static_cast<meter::ActorType>(packet.actor_type),
                packet.max_hp,
                packet.current_hp
            );
        }

        if (m_ring_buffer) {
            std::vector<uint8_t> bytes = ipc::serialize_typed_packet(
                PluginId::CombatMeter, MessageType::CombatActorInfo, 0, packet
            );
            m_ring_buffer->push(bytes);
        }
    }
}

void ObjectReader::sync_party(meter::CombatantRegistry* registry) {
    if (!m_initialized && !initialize()) return;
    if (m_group_manager_addr == 0) return;

    ipc::PartySyncPacket sync{};
    ExtractedParty extracted{};
    if (!SafeReadParty(m_group_manager_addr, sync, extracted)) {
        return;
    }

    // Party members carry name/job/HP that the action hook only learns once an
    // actor has acted, so publish them as actor info too.
    for (uint8_t i = 0; i < extracted.count; ++i) {
        const auto& m = extracted.members[i];
        if (m.entity_id == 0) continue;

        bool actor_changed = false;
        {
            std::lock_guard<std::mutex> lock(m_cache_mutex);
            auto it = m_actor_cache.find(m.entity_id);
            if (it == m_actor_cache.end() || it->second.job_id != m.job_id || it->second.name != m.name) {
                m_actor_cache[m.entity_id] = CachedActor{0, m.job_id, m.max_hp, m.name};
                actor_changed = true;
            }
        }
        if (!actor_changed) continue;

        ipc::ActorInfoPacket actor{};
        actor.entity_id = m.entity_id;
        actor.job_id = m.job_id;
        actor.current_hp = m.current_hp;
        actor.max_hp = m.max_hp;
        actor.actor_type = static_cast<uint8_t>(meter::ActorType::Player);
        std::memcpy(actor.name, m.name, sizeof(actor.name) - 1);
        actor.name[sizeof(actor.name) - 1] = '\0';

        if (registry) {
            registry->register_actor(
                actor.entity_id,
                actor.name,
                static_cast<meter::Job>(actor.job_id),
                0,
                meter::ActorType::Player,
                actor.max_hp,
                actor.current_hp
            );
        }
        if (m_ring_buffer) {
            m_ring_buffer->push(ipc::serialize_typed_packet(
                PluginId::CombatMeter, MessageType::CombatActorInfo, 0, actor
            ));
        }
    }

    bool party_changed = false;
    {
        std::lock_guard<std::mutex> lock(m_cache_mutex);
        if (sync.party_count != m_last_party_sync.party_count) {
            party_changed = true;
        } else {
            for (uint32_t i = 0; i < sync.party_count; ++i) {
                if (sync.entity_ids[i] != m_last_party_sync.entity_ids[i] ||
                    sync.job_ids[i] != m_last_party_sync.job_ids[i]) {
                    party_changed = true;
                    break;
                }
            }
        }
        if (party_changed) {
            m_last_party_sync = sync;
        }
    }

    if (registry) {
        registry->sync_party(sync);
    }

    if (party_changed && m_ring_buffer) {
        m_ring_buffer->push(ipc::serialize_typed_packet(
            PluginId::CombatMeter, MessageType::CombatPartySync, 0, sync
        ));
    }
}

} // namespace hub::payload

#else // !_WIN32 - Cross-platform mock implementation

namespace hub::payload {

ObjectReader::ObjectReader(RingBuffer* ring_buffer)
    : m_ring_buffer(ring_buffer) {}

bool ObjectReader::initialize() {
    m_initialized = true;
    return true;
}

bool ObjectReader::read_character(uint32_t, ipc::ActorInfoPacket&) {
    return false;
}

void ObjectReader::inspect_and_sync_actor(uint32_t, meter::CombatantRegistry*) {}
void ObjectReader::inspect_and_sync_actor_direct(void*, meter::CombatantRegistry*) {}
void ObjectReader::sync_party(meter::CombatantRegistry*) {}

} // namespace hub::payload

#endif
