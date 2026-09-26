#include "payload/object_reader.hpp"
#include "meter/actor_info.hpp"
#include "meter/buff_attribution.hpp"
#include "meter/combatant_registry.hpp"
#include "common/sigscan.hpp"
#include "common/pe_scanner.hpp"
#include "common/os/logger.hpp"
#include "common/os/safe_memory.hpp"
#include "hub/game/entity.hpp"
#include "hub/game/raid_buffs.hpp"
#include "hub/game/status.hpp"
#include <chrono>
#include <cstring>

namespace hub::payload {

namespace detail {

namespace {

/// The layout check both status reads share: a slot count of 30 or 60, and, with
/// `expected_owner` set, an owner that is null or that object.
bool status_layout_ok(const game::StatusManagerObject& manager, const void* expected_owner) noexcept {
    const uint8_t slots = manager.slot_count;
    if (slots != game::definitions::DEFAULT_STATUS_SLOTS && slots != game::definitions::MAX_STATUS_SLOTS) {
        return false;
    }
    return expected_owner == nullptr || manager.owner == nullptr || manager.owner == expected_owner;
}

} // namespace

bool extract_status_list(const game::StatusManagerObject* manager, const void* expected_owner,
                         bool with_detail, meter::ActorVitals& out) noexcept {
    out.count = 0;
    out.statuses_read = false;
    out.status_detail = with_detail;
    if (manager == nullptr || !status_layout_ok(*manager, expected_owner)) return false;

    for (size_t i = 0; i < manager->slot_count && i < game::definitions::MAX_STATUS_SLOTS; ++i) {
        const game::StatusEntry& slot = manager->statuses[i];
        if (slot.status_id == 0) continue;
        ipc::CombatStatusEntry& entry = out.entries[out.count++];
        entry.status_id = slot.status_id;
        entry.param = slot.param;
        entry.remaining_s = with_detail ? slot.remaining : 0.0f;
        entry.source_id = with_detail ? static_cast<uint32_t>(slot.source_id) : 0;
    }
    out.statuses_read = true;
    return true;
}

bool extract_attribution_statuses(const game::StatusManagerObject* manager, const void* expected_owner,
                                  ipc::CombatStatusEntry* out, size_t capacity, size_t& count) noexcept {
    count = 0;
    if (manager == nullptr || out == nullptr || !status_layout_ok(*manager, expected_owner)) return false;
    for (size_t i = 0; i < manager->slot_count && i < game::definitions::MAX_STATUS_SLOTS && count < capacity; ++i) {
        const game::StatusEntry& slot = manager->statuses[i];
        if (slot.status_id == 0 || !meter::is_attribution_status(slot.status_id)) continue;
        ipc::CombatStatusEntry& entry = out[count++];
        entry.status_id = slot.status_id;
        entry.param = slot.param;
        entry.remaining_s = slot.remaining;
        entry.source_id = static_cast<uint32_t>(slot.source_id);
    }
    return true;
}

bool reads_as_lobby(const uint32_t* local_player_id) noexcept {
    return local_player_id != nullptr && *local_player_id == hub::game::NO_ENTITY_ID;
}

} // namespace detail

ObjectReader::ObjectReader(RingBuffer* ring_buffer)
    : m_ring_buffer(ring_buffer) {}

bool ObjectReader::read_character_object(const void* character_ptr, ipc::ActorInfoPacket& out_packet) {
    return meter::read_actor_info(character_ptr, out_packet);
}

void ObjectReader::note_status_layout(bool ok) {
    if (ok) {
        m_status_layout_confirmed = true;
        m_status_layout_failures = 0;
        return;
    }
    // One odd object after reads have worked is a transient, not a moved layout.
    if (m_status_layout_confirmed || m_status_reads_disabled.load(std::memory_order_relaxed)) return;
    if (++m_status_layout_failures >= kStatusLayoutStrikes) {
        m_status_reads_disabled.store(true, std::memory_order_relaxed);
        hub::os::Logger::warn(
            "Status reads switched off: no StatusManager read passed the layout check. "
            "Its offsets in game_definitions.hpp need re-verifying for this game version.");
    }
}

bool ObjectReader::publish_actor(const ipc::ActorInfoPacket& packet, meter::CombatantRegistry* registry) {
    bool changed = true;
    {
        std::lock_guard<std::mutex> lock(m_cache_mutex);
        const bool dead = is_dead(packet.current_hp, packet.max_hp);
        auto it = m_actor_cache.find(packet.entity_id);
        if (it != m_actor_cache.end()) {
            changed = it->second.owner_id != packet.owner_id ||
                      it->second.job_id != packet.job_id ||
                      it->second.max_hp != packet.max_hp ||
                      it->second.name != packet.name ||
                      it->second.dead != dead;
        }
        m_actor_cache[packet.entity_id] = CachedActor{
            packet.owner_id, packet.job_id, packet.max_hp, packet.name,
            std::chrono::steady_clock::now(), dead
        };
    }
    if (!changed) {
        return false;
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
        m_ring_buffer->push(ipc::serialize_typed_packet(
            PluginId::CombatMeter, MessageType::CombatActorInfo, 0, packet
        ));
    }
    return true;
}

void ObjectReader::inspect_and_sync_actor_direct(const void* character_ptr, meter::CombatantRegistry* registry) {
    if (!character_ptr) return;
    ipc::ActorInfoPacket packet{};
    // The action hook fires several times a second per actor, so this goes
    // through the same cache as the object-table path rather than pushing an
    // identical packet on every effect.
    if (read_character_object(character_ptr, packet)) {
        publish_actor(packet, registry);
    }
}

void ObjectReader::invalidate_cache() {
    std::lock_guard<std::mutex> lock(m_cache_mutex);
    m_actor_cache.clear();
    m_last_party_sync = ipc::PartySyncPacket{};
    m_last_territory = 0;
    m_territory_published = false;
}

} // namespace hub::payload

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace hub::payload {

namespace {

using FnGetObjectByEntityId = game::CharacterObject*(void*, uint32_t);

// Game memory is copied out through hub::os::safe_read, which carries the SEH guard,
// and checked on the copy. Only the call into the game's own lookup has a guard here.

static const game::CharacterObject* SafeGetObjectByEntityId(
    FnGetObjectByEntityId* fn, uintptr_t game_obj_mgr_addr, uint32_t entity_id
) {
    __try {
        if (fn == nullptr || game_obj_mgr_addr == 0 || !game::is_real_entity_id(entity_id)) return nullptr;
        return fn(reinterpret_cast<void*>(game_obj_mgr_addr + game::offsets::GAME_OBJECT_MANAGER_OBJECT_ARRAYS), entity_id);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool SafeReadCharacter(
    FnGetObjectByEntityId* fn,
    uintptr_t game_obj_mgr_addr,
    uint32_t entity_id,
    ipc::ActorInfoPacket& out
) {
    const game::CharacterObject* obj = SafeGetObjectByEntityId(fn, game_obj_mgr_addr, entity_id);
    return obj != nullptr && meter::read_actor_info(obj, out);
}

/// The main group of the party list, copied out in one read.
struct PartyCopy {
    uint8_t count{0};
    uintptr_t main_group{0};
    game::PartyMemberObject members[game::definitions::MAX_PARTY_MEMBERS];

    [[nodiscard]] uintptr_t member_addr(uint8_t i) const noexcept {
        return main_group + (i * game::offsets::PARTY_MEMBER_SIZE);
    }
};

/// The count is clamped to MAX_PARTY_MEMBERS. False when the read faulted.
bool SafeReadParty(uintptr_t group_mgr_addr, PartyCopy& out) {
    out.count = 0;
    if (group_mgr_addr == 0) return false;

    // GroupManager is the static instance itself, not a pointer to one.
    out.main_group = group_mgr_addr + game::offsets::GROUP_MAIN_GROUP;
    uint8_t count = 0;
    if (!hub::os::safe_read(out.main_group + game::offsets::GROUP_MEMBER_COUNT, count)) return false;
    if (count > game::definitions::MAX_PARTY_MEMBERS) {
        count = static_cast<uint8_t>(game::definitions::MAX_PARTY_MEMBERS);
    }
    if (!hub::os::safe_copy(out.members, reinterpret_cast<const void*>(out.main_group),
                            count * sizeof(game::PartyMemberObject))) {
        return false;
    }
    out.count = count;
    return true;
}

/// A cross-world member standing somewhere else must not redefine where the party
/// is, so take the territory most of the list agrees on. 0 when none reports one.
uint16_t party_territory(const PartyCopy& party) {
    uint16_t best = 0;
    uint8_t best_votes = 0;
    for (uint8_t i = 0; i < party.count; ++i) {
        const uint16_t territory = party.members[i].territory_type;
        if (territory == 0) continue;
        uint8_t votes = 0;
        for (uint8_t j = 0; j < party.count; ++j) {
            if (party.members[j].territory_type == territory) ++votes;
        }
        if (votes > best_votes) {
            best_votes = votes;
            best = territory;
        }
    }
    return best;
}

uint32_t SafeReadLocalPlayerId(uintptr_t addr) {
    uint32_t id = 0;
    return hub::os::safe_read(addr, id) && game::is_real_entity_id(id) ? id : 0;
}

bool SafeReadInLobby(uintptr_t addr) {
    uint32_t id = 0;
    return hub::os::safe_read(addr, id) && detail::reads_as_lobby(&id);
}

/// The BattleChara for `entity_id`: a player or a battle NPC whose id reads back
/// the same. The lookup is a binary search the main thread may be reshaping, so
/// the id is checked again on the object it returns.
const game::CharacterObject* SafeFindBattleChara(
    FnGetObjectByEntityId* fn, uintptr_t game_obj_mgr_addr, uint32_t entity_id, uint32_t& hp, uint32_t& max_hp
) {
    const game::CharacterObject* obj = SafeGetObjectByEntityId(fn, game_obj_mgr_addr, entity_id);
    game::CharacterObject copy;
    if (obj == nullptr || !hub::os::safe_read(obj, copy)) return nullptr;
    if (copy.entity_id != entity_id || !game::is_battle_chara_kind(copy.object_kind)) return nullptr;
    hp = copy.current_hp;
    max_hp = copy.max_hp;
    return obj;
}

/// The local player's own Character, which the client keeps next to its id.
const game::CharacterObject* SafeReadLocalPlayerObject(
    uintptr_t id_addr, uint32_t& entity_id, uint32_t& hp, uint32_t& max_hp
) {
    uint32_t id = 0;
    if (!hub::os::safe_read(id_addr, id) || !game::is_real_entity_id(id)) return nullptr;
    const game::CharacterObject* obj = nullptr;
    if (!hub::os::safe_read(id_addr + game::definitions::LOCAL_PLAYER_OBJECT_FROM_ID, obj)) return nullptr;
    game::CharacterObject copy;
    if (obj == nullptr || !hub::os::safe_read(obj, copy)) return nullptr;
    if (copy.entity_id != id || copy.object_kind != game::ObjectKind::Player) return nullptr;
    entity_id = id;
    hp = copy.current_hp;
    max_hp = copy.max_hp;
    return obj;
}

/// False when the read faulted, which says nothing about the layout. Otherwise
/// `layout_ok` carries the layout check's verdict.
bool SafeExtractStatusList(
    uintptr_t manager_addr, const void* expected_owner, bool with_detail, meter::ActorVitals& out, bool& layout_ok
) {
    game::StatusManagerObject manager;
    if (!hub::os::safe_read(manager_addr, manager)) {
        out.count = 0;
        out.statuses_read = false;
        return false;
    }
    layout_ok = detail::extract_status_list(&manager, expected_owner, with_detail, out);
    return true;
}

/// The StatusManager of a BattleChara the hook handed over, once its id and kind check out.
bool SafeExtractAttributionStatuses(
    const game::CharacterObject* obj, uint32_t entity_id, ipc::CombatStatusEntry* out, size_t capacity, size_t& count
) {
    count = 0;
    game::CharacterObject copy;
    if (obj == nullptr || !hub::os::safe_read(obj, copy)) return false;
    if (copy.entity_id != entity_id || !game::is_battle_chara_kind(copy.object_kind)) return false;
    game::StatusManagerObject manager;
    if (!hub::os::safe_read(reinterpret_cast<uintptr_t>(obj) + game::offsets::BATTLE_CHARA_STATUS_MANAGER, manager)) {
        return false;
    }
    return detail::extract_attribution_statuses(&manager, obj, out, capacity, count);
}

} // namespace

std::optional<size_t> ObjectReader::read_attribution_statuses(
    uint32_t entity_id, const void* character, std::span<ipc::CombatStatusEntry> out
) {
    if (!m_initialized || !status_reads_enabled() || !game::is_real_entity_id(entity_id)) return std::nullopt;

    const auto* obj = static_cast<const game::CharacterObject*>(character);
    if (obj == nullptr) {
        uint32_t hp = 0;
        uint32_t max_hp = 0;
        obj = SafeFindBattleChara(m_fp_get_object_by_id, m_game_object_mgr_addr, entity_id, hp, max_hp);
        if (obj == nullptr) return std::nullopt;
    }
    size_t count = 0;
    if (!SafeExtractAttributionStatuses(obj, entity_id, out.data(), out.size(), count)) return std::nullopt;

    // Whether the status carries the steps danced or codas sung is not in the client.
    // Logged once per value so a live session can settle it.
    for (size_t i = 0; i < count; ++i) {
        const game::RaidBuff* buff = game::find_raid_buff(out[i].status_id);
        if (buff == nullptr || buff->value != game::RaidBuffValue::ByApplication) continue;
        const uint32_t key = (static_cast<uint32_t>(out[i].status_id) << 16) | out[i].param;
        if (m_logged_strength_params.insert(key).second) {
            hub::os::Logger::info("Status " + std::to_string(out[i].status_id) + " (" +
                                  game::status_name(out[i].status_id) + ") seen with param " +
                                  std::to_string(out[i].param));
        }
    }
    return count;
}

size_t ObjectReader::read_vitals(std::span<const uint32_t> enemy_ids, std::span<meter::ActorVitals> out) {
    if (!m_initialized || out.empty()) return 0;

    const auto read_statuses = [this](meter::ActorVitals& vitals, uintptr_t manager_addr, const void* owner, bool detail) {
        if (m_status_reads_disabled.load(std::memory_order_relaxed)) return;
        bool layout_ok = false;
        if (SafeExtractStatusList(manager_addr, owner, detail, vitals, layout_ok)) {
            note_status_layout(layout_ok);
        }
    };
    const auto own_statuses = [](const game::CharacterObject* obj) {
        return reinterpret_cast<uintptr_t>(obj) + game::offsets::BATTLE_CHARA_STATUS_MANAGER;
    };

    size_t filled = 0;
    PartyCopy party;
    const uint8_t party_count = SafeReadParty(m_group_manager_addr, party) ? party.count : 0;
    for (uint8_t i = 0; i < party_count && filled < out.size(); ++i) {
        const game::PartyMemberObject& slot = party.members[i];
        if (!game::is_real_entity_id(slot.entity_id)) continue;
        meter::ActorVitals& vitals = out[filled++];
        vitals = meter::ActorVitals{};
        vitals.entity = slot.entity_id;
        // The party list's HP, the same reading the wipe check uses.
        vitals.hp = slot.current_hp;
        vitals.max_hp = slot.max_hp;
        vitals.track_life = true;
        uint32_t hp = 0;
        uint32_t max_hp = 0;
        if (const auto* obj = SafeFindBattleChara(m_fp_get_object_by_id, m_game_object_mgr_addr, slot.entity_id, hp, max_hp)) {
            read_statuses(vitals, own_statuses(obj), obj, true);
        }
        if (!vitals.statuses_read) {
            // Out of range: the list's own copy, which has no timers or sources.
            read_statuses(vitals, party.member_addr(i) + game::offsets::PARTY_MEMBER_STATUS_MANAGER, nullptr, false);
        }
    }

    // Solo, the party list is empty and the local player is only in the object table.
    if (party_count == 0 && filled < out.size()) {
        uint32_t id = 0;
        uint32_t hp = 0;
        uint32_t max_hp = 0;
        if (const auto* obj = SafeReadLocalPlayerObject(m_local_player_id_addr, id, hp, max_hp)) {
            meter::ActorVitals& vitals = out[filled++];
            vitals = meter::ActorVitals{};
            vitals.entity = id;
            vitals.hp = hp;
            vitals.max_hp = max_hp;
            vitals.track_life = true;
            read_statuses(vitals, own_statuses(obj), obj, true);
        }
    }

    for (const uint32_t id : enemy_ids) {
        if (filled >= out.size()) break;
        uint32_t hp = 0;
        uint32_t max_hp = 0;
        const auto* obj = SafeFindBattleChara(m_fp_get_object_by_id, m_game_object_mgr_addr, id, hp, max_hp);
        if (obj == nullptr) continue;
        meter::ActorVitals& vitals = out[filled++];
        vitals = meter::ActorVitals{};
        vitals.entity = id;
        vitals.hp = hp;
        vitals.max_hp = max_hp;
        vitals.is_enemy = true;
        read_statuses(vitals, own_statuses(obj), obj, true);
    }
    return filled;
}

bool ObjectReader::initialize() {
    if (m_initialized) return true;

    HMODULE h_game = GetModuleHandleW(nullptr);
    if (!h_game) return false;
    namespace defs = game::definitions;

    // Scan for GameObjectManager
    uintptr_t mgr_ins = common::pe::scan_module_section(h_game, ".text", game::signatures::GAME_OBJECT_MANAGER_INSTANCE);
    if (mgr_ins) {
        m_game_object_mgr_addr = hub::memory::resolve_rip_relative(
            mgr_ins, defs::GAME_OBJECT_MANAGER_INSTANCE_RIP_DISP_OFFSET, defs::GAME_OBJECT_MANAGER_INSTANCE_RIP_INSN_END);
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
        m_group_manager_addr = hub::memory::resolve_rip_relative(
            group_ins, defs::GROUP_MANAGER_INSTANCE_RIP_DISP_OFFSET, defs::GROUP_MANAGER_INSTANCE_RIP_INSN_END);
    }

    // Local player entity id: the party list cannot say which slot is us.
    if (uintptr_t ins = common::pe::scan_module_section(h_game, ".text", game::signatures::LOCAL_PLAYER_ENTITY_ID_PRIMARY)) {
        m_local_player_id_addr = hub::memory::resolve_rip_relative(
            ins, defs::LOCAL_PLAYER_ENTITY_ID_PRIMARY_RIP_DISP_OFFSET, defs::LOCAL_PLAYER_ENTITY_ID_PRIMARY_RIP_INSN_END);
    } else if (uintptr_t fb = common::pe::scan_module_section(h_game, ".text", game::signatures::LOCAL_PLAYER_ENTITY_ID_FALLBACK)) {
        m_local_player_id_addr = hub::memory::resolve_rip_relative(
            fb, defs::LOCAL_PLAYER_ENTITY_ID_FALLBACK_RIP_DISP_OFFSET, defs::LOCAL_PLAYER_ENTITY_ID_FALLBACK_RIP_INSN_END);
    }

    m_initialized = (m_game_object_mgr_addr != 0 || m_group_manager_addr != 0);
    return m_initialized;
}

bool ObjectReader::read_character(uint32_t entity_id, ipc::ActorInfoPacket& out_packet) {
    if (!m_initialized) return false;
    return SafeReadCharacter(m_fp_get_object_by_id, m_game_object_mgr_addr, entity_id, out_packet);
}

bool ObjectReader::in_lobby() const {
    return SafeReadInLobby(m_local_player_id_addr);
}

void ObjectReader::inspect_and_sync_actor(uint32_t entity_id, meter::CombatantRegistry* registry) {
    if (!game::is_real_entity_id(entity_id)) {
        return;
    }

    // This runs per decoded effect. The cache is keyed on when the object table
    // was last read, not on the actor being "identified": monsters and NPCs have
    // job_id 0 permanently, so that test never passed for them and every hit on
    // a boss re-ran the lookup and re-sent an ActorInfo packet.
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(m_cache_mutex);
        auto it = m_actor_cache.find(entity_id);
        if (it != m_actor_cache.end() && (now - it->second.last_read) < kActorCacheTtl) {
            return;
        }
    }

    ipc::ActorInfoPacket packet{};
    if (read_character(entity_id, packet)) {
        publish_actor(packet, registry);
    }
}

void ObjectReader::sync_party(meter::CombatantRegistry* registry) {
    if (!m_initialized) return;
    if (m_group_manager_addr == 0) return;

    PartyCopy party;
    if (!SafeReadParty(m_group_manager_addr, party)) {
        return;
    }
    ipc::PartySyncPacket sync{};
    sync.party_count = party.count;
    sync.local_player_id = SafeReadLocalPlayerId(m_local_player_id_addr);

    // Party members carry name/job/HP that the action hook only learns once an
    // actor has acted, so publish them as actor info too.
    for (uint8_t i = 0; i < party.count; ++i) {
        const game::PartyMemberObject& m = party.members[i];
        sync.entity_ids[i] = m.entity_id;
        sync.job_ids[i] = m.class_job;
        if (m.entity_id == 0) continue;

        // The local engine takes every reading; only a death or raise goes on the wire,
        // through publish_actor's dedupe.
        if (registry) {
            registry->update_hp(m.entity_id, m.current_hp, m.max_hp);
        }

        ipc::ActorInfoPacket actor{};
        actor.entity_id = m.entity_id;
        actor.job_id = m.class_job;
        actor.current_hp = m.current_hp;
        actor.max_hp = m.max_hp;
        actor.actor_type = static_cast<uint8_t>(meter::ActorType::Player);
        std::memcpy(actor.name, m.name, sizeof(actor.name) - 1);
        actor.name[sizeof(actor.name) - 1] = '\0';
        publish_actor(actor, registry);
    }

    // Solo the list is empty, and the local player's own object is the only HP the
    // wipe check can read. Published like a member, so a death or raise reaches the app.
    if (party.count == 0) {
        uint32_t self_id = 0;
        uint32_t self_hp = 0;
        uint32_t self_max_hp = 0;
        const auto* self = SafeReadLocalPlayerObject(m_local_player_id_addr, self_id, self_hp, self_max_hp);
        ipc::ActorInfoPacket actor{};
        if (self != nullptr && meter::read_actor_info(self, actor)) {
            publish_actor(actor, registry);
            if (registry) {
                registry->update_hp(actor.entity_id, actor.current_hp, actor.max_hp);
            }
        }
    }

    bool party_changed = false;
    {
        std::lock_guard<std::mutex> lock(m_cache_mutex);
        if (sync.party_count != m_last_party_sync.party_count ||
            sync.local_player_id != m_last_party_sync.local_player_id) {
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

    // The party list is the only place the payload can see a territory id. An
    // empty list means solo, so the zone becomes unknown instead of staying on
    // whatever duty the last party was in; a list with no territory tells nothing.
    const uint16_t territory = party_territory(party);
    const bool territory_known = party.count == 0 || territory != 0;
    bool territory_changed = false;
    {
        std::lock_guard<std::mutex> lock(m_cache_mutex);
        if (territory_known &&
            (!m_territory_published || territory != m_last_territory)) {
            m_last_territory = territory;
            m_territory_published = true;
            territory_changed = true;
            // Nothing from the old zone's object table is coming back, and the
            // cache would otherwise keep every actor id seen this session.
            m_actor_cache.clear();
        }
    }

    if (territory_changed && m_ring_buffer) {
        // in_combat_flag and control_command stay 0: this packet only announces
        // the zone (0 = unknown), it must not start or end an encounter on the app side.
        ipc::EncounterControlPacket ctrl{};
        ctrl.zone_id = territory;
        ctrl.timestamp_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
        m_ring_buffer->push(ipc::serialize_typed_packet(
            PluginId::CombatMeter, MessageType::CombatControl, 0, ctrl
        ));
    }
}

} // namespace hub::payload

#else // !_WIN32 - Cross-platform mock implementation

namespace hub::payload {

bool ObjectReader::initialize() {
    m_initialized = true;
    return true;
}

bool ObjectReader::read_character(uint32_t, ipc::ActorInfoPacket&) {
    return false;
}

bool ObjectReader::in_lobby() const {
    return false;
}

void ObjectReader::inspect_and_sync_actor(uint32_t, meter::CombatantRegistry*) {}
void ObjectReader::sync_party(meter::CombatantRegistry*) {}

std::optional<size_t> ObjectReader::read_attribution_statuses(
    uint32_t, const void*, std::span<ipc::CombatStatusEntry>
) {
    return std::nullopt;
}

size_t ObjectReader::read_vitals(std::span<const uint32_t>, std::span<meter::ActorVitals>) {
    return 0;
}

} // namespace hub::payload

#endif
