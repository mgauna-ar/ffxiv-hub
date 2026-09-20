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

static bool SafeReadParty(
    uintptr_t group_mgr_addr,
    ipc::PartySyncPacket& out
) {
    __try {
        if (group_mgr_addr == 0) return false;

        uintptr_t group_mgr = *reinterpret_cast<uintptr_t*>(group_mgr_addr);
        if (group_mgr == 0) return false;

        const uint32_t count = *reinterpret_cast<const uint32_t*>(group_mgr + game::offsets::GROUP_MEMBER_COUNT);
        if (count == 0 || count > game::definitions::MAX_PARTY_MEMBERS) {
            out.party_count = 0;
            return true;
        }

        out.party_count = count;
        uintptr_t party_base = group_mgr + game::offsets::GROUP_MAIN_GROUP;

        for (uint32_t i = 0; i < count; ++i) {
            uintptr_t member_addr = party_base + (i * game::offsets::PARTY_MEMBER_SIZE);
            out.entity_ids[i] = *reinterpret_cast<const uint32_t*>(member_addr + game::offsets::PARTY_MEMBER_ENTITY_ID);
            out.job_ids[i] = *reinterpret_cast<const uint8_t*>(member_addr + game::offsets::PARTY_MEMBER_CLASS_JOB);
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
        m_game_object_mgr_addr = common::sigscan::resolve_rip_relative(mgr_ins, 3, 7);
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
        m_group_manager_addr = common::sigscan::resolve_rip_relative(group_ins, 5, 9);
    }

    m_initialized = (m_game_object_mgr_addr != 0 || m_group_manager_addr != 0);
    return m_initialized;
}

bool ObjectReader::read_character(uint32_t entity_id, ipc::ActorInfoPacket& out_packet) {
    if (!m_initialized && !initialize()) return false;
    return SafeReadCharacter(m_fp_get_object_by_id, m_game_object_mgr_addr, entity_id, out_packet);
}

void ObjectReader::inspect_and_sync_actor(uint32_t entity_id, meter::CombatantRegistry* registry) {
    ipc::ActorInfoPacket packet{};
    if (read_character(entity_id, packet)) {
        if (registry) {
            registry->register_actor(
                packet.entity_id,
                packet.name,
                static_cast<meter::Job>(packet.job_id),
                static_cast<meter::ActorType>(packet.actor_type),
                packet.owner_id
            );
        }

        if (m_ring_buffer) {
            std::vector<uint8_t> bytes = ipc::serialize_packet(
                PluginId::CombatMeter, MessageType::CombatActorInfo, 0, packet
            );
            m_ring_buffer->try_push(bytes);
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
                static_cast<meter::ActorType>(packet.actor_type),
                packet.owner_id
            );
        }

        if (m_ring_buffer) {
            std::vector<uint8_t> bytes = ipc::serialize_packet(
                PluginId::CombatMeter, MessageType::CombatActorInfo, 0, packet
            );
            m_ring_buffer->try_push(bytes);
        }
    }
}

void ObjectReader::sync_party(meter::CombatantRegistry* registry) {
    if (!m_initialized && !initialize()) return;
    if (m_group_manager_addr == 0) return;

    ipc::PartySyncPacket sync{};
    if (SafeReadParty(m_group_manager_addr, sync)) {
        if (registry) {
            std::vector<uint32_t> members;
            members.reserve(sync.party_count);
            for (uint32_t i = 0; i < sync.party_count; ++i) {
                members.push_back(sync.entity_ids[i]);
            }
            registry->sync_party(members);
        }

        if (m_ring_buffer) {
            std::vector<uint8_t> bytes = ipc::serialize_packet(
                PluginId::CombatMeter, MessageType::CombatPartySync, 0, sync
            );
            m_ring_buffer->try_push(bytes);
        }
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
