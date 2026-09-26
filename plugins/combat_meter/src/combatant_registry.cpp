#include "meter/combatant_registry.hpp"
#include "hub/game/entity.hpp"
#include "hub/game/limit_break.hpp"
#include "hub/game/pets.hpp"
#include <cstring>
#include <algorithm>
#include <cctype>

namespace hub::meter {

bool CombatantRegistry::is_known_pet_name(std::string_view name) {
    return hub::game::is_known_pet_name(name);
}

Job CombatantRegistry::infer_pet_job(std::string_view name) {
    return hub::game::infer_pet_job(name);
}

void CombatantRegistry::register_actor(const ipc::ActorInfoPacket& packet) {
    char name_buf[constants::MAX_NAME_LENGTH + 1] = {0};
    std::memcpy(name_buf, packet.name, constants::MAX_NAME_LENGTH);
    Combatant& actor = register_actor(packet.entity_id, std::string(name_buf),
                                      static_cast<Job>(packet.job_id), packet.owner_id,
                                      static_cast<ActorType>(packet.actor_type),
                                      packet.max_hp, packet.current_hp);
    actor.world_id = packet.world_id;
}

Combatant& CombatantRegistry::register_actor(
    EntityId entity_id,
    std::string name,
    Job job,
    EntityId owner_id,
    ActorType actor_type,
    uint32_t max_hp,
    uint32_t current_hp
) {
    if (name.empty()) {
        name = "Actor_" + std::to_string(entity_id);
    }

    const bool pet_by_name = is_known_pet_name(name);
    const bool is_pet_actor = (owner_id != 0) || pet_by_name;

    if (job == Job::None && is_pet_actor) {
        job = infer_pet_job(name);
    }

    if (actor_type == ActorType::Unknown) {
        if (is_pet_actor) {
            actor_type = ActorType::Pet;
        } else if (job_to_role(job) != Role::None) {
            actor_type = ActorType::Player;
        }
    }

    EntityId resolved_owner_id = owner_id;
    if (resolved_owner_id == 0 && is_pet_actor) {
        const Job pet_job = (job != Job::None) ? job : infer_pet_job(name);
        if (pet_job != Job::None) {
            if (m_local_player_id != 0) {
                auto lp = m_actors.find(m_local_player_id);
                if (lp != m_actors.end() && lp->second.job == pet_job) {
                    resolved_owner_id = m_local_player_id;
                }
            }
            if (resolved_owner_id == 0) {
                EntityId candidate = 0;
                size_t matches = 0;
                for (EntityId pm_id : m_party_members) {
                    auto pm = m_actors.find(pm_id);
                    if (pm != m_actors.end() && pm->second.job == pet_job) {
                        candidate = pm_id;
                        matches++;
                    }
                }
                if (matches == 1) {
                    resolved_owner_id = candidate;
                }
            }
        }
    }

    Combatant& actor = m_actors[entity_id];
    actor.entity_id = entity_id;
    actor.owner_id = resolved_owner_id;
    actor.name = std::move(name);
    actor.job = job;
    actor.role = job_to_role(job);
    actor.actor_type = actor_type;
    actor.max_hp = max_hp;
    // 0 of a known max is a dead actor, not a missing reading; is_party_wiped()
    // tells the two apart by max_hp.
    actor.current_hp = current_hp;
    actor.is_pet = is_pet_actor;
    actor.is_party_member = is_party_member(entity_id);
    actor.is_local_player = (entity_id == m_local_player_id);

    if (resolved_owner_id != 0) {
        m_pet_to_owner[entity_id] = resolved_owner_id;
    } else if (!is_pet_actor) {
        // A recycled pet id must not keep merging into the old owner.
        m_pet_to_owner.erase(entity_id);
    }

    return actor;
}

void CombatantRegistry::set_pet_owner(EntityId pet_id, EntityId owner_id) {
    m_pet_to_owner[pet_id] = owner_id;
    auto it = m_actors.find(pet_id);
    if (it != m_actors.end()) {
        it->second.owner_id = owner_id;
        it->second.is_pet = true;
    }
}

EntityId CombatantRegistry::resolve_owner(EntityId entity_id) const {
    EntityId curr = entity_id;
    int depth = 0;
    while (depth < 8) {
        auto it = m_pet_to_owner.find(curr);
        if (it != m_pet_to_owner.end() && it->second != 0 && it->second != curr) {
            curr = it->second;
            depth++;
        } else {
            break;
        }
    }
    return curr;
}

bool CombatantRegistry::is_pet(EntityId entity_id) const {
    auto it = m_pet_to_owner.find(entity_id);
    if (it != m_pet_to_owner.end() && it->second != 0) {
        return true;
    }
    auto act = m_actors.find(entity_id);
    if (act != m_actors.end()) {
        return act->second.is_pet;
    }
    return false;
}

const Combatant* CombatantRegistry::find_actor(EntityId entity_id) const {
    auto it = m_actors.find(entity_id);
    if (it != m_actors.end()) {
        return &it->second;
    }
    return nullptr;
}

Combatant* CombatantRegistry::find_actor_mut(EntityId entity_id) {
    auto it = m_actors.find(entity_id);
    if (it != m_actors.end()) {
        return &it->second;
    }
    return nullptr;
}

Combatant& CombatantRegistry::get_or_create(EntityId entity_id, std::string_view default_name) {
    auto it = m_actors.find(entity_id);
    if (it != m_actors.end()) {
        return it->second;
    }

    std::string name = default_name.empty() ? ("Actor_" + std::to_string(entity_id)) : std::string(default_name);
    return register_actor(entity_id, std::move(name));
}

void CombatantRegistry::sync_party(const ipc::PartySyncPacket& packet) {
    m_party_members.clear();
    const uint32_t count = std::min(packet.party_count, static_cast<uint32_t>(ipc::MAX_PARTY_MEMBERS));

    for (uint32_t i = 0; i < count; ++i) {
        const EntityId id = packet.entity_ids[i];
        if (id == 0) continue;

        m_party_members.push_back(id);
        auto it = m_actors.find(id);
        if (it != m_actors.end()) {
            if (it->second.job == Job::None && packet.job_ids[i] != 0) {
                it->second.job = static_cast<Job>(packet.job_ids[i]);
                it->second.role = job_to_role(it->second.job);
            }
        } else if (packet.job_ids[i] != 0) {
            Combatant& new_actor = get_or_create(id);
            new_actor.job = static_cast<Job>(packet.job_ids[i]);
            new_actor.role = job_to_role(new_actor.job);
        }
    }

    // Recomputed for everyone, not just set on the current list: a member who
    // left would otherwise stay a party member for the rest of the session.
    for (auto& [id, actor] : m_actors) {
        actor.is_party_member = is_party_member(id);
    }

    // Recomputed every sync rather than latched once. Taken from the packet, never
    // from slot 0: the party list is in server order, so slot 0 is often someone else.
    set_local_player(hub::game::is_real_entity_id(packet.local_player_id) ? packet.local_player_id : 0);
}

void CombatantRegistry::set_party_members(const std::vector<EntityId>& member_ids) {
    m_party_members = member_ids;
    for (auto& [id, actor] : m_actors) {
        actor.is_party_member = is_party_member(id);
    }
}

bool CombatantRegistry::is_party_member(EntityId entity_id) const {
    return std::find(m_party_members.begin(), m_party_members.end(), entity_id) != m_party_members.end();
}

bool CombatantRegistry::is_friendly(EntityId entity_id) const {
    if (entity_id == 0) {
        return false;
    }
    // Checked before the monster-bit test below, which the synthetic id also matches.
    if (entity_id == hub::game::LIMIT_BREAK_COMBATANT_ID) {
        return true;
    }
    if (is_party_member(entity_id)) {
        return true;
    }
    if (m_local_player_id != 0 && entity_id == m_local_player_id) {
        return true;
    }
    if (is_pet(entity_id)) {
        return true;
    }

    auto it = m_actors.find(entity_id);
    if (it != m_actors.end()) {
        const auto& actor = it->second;
        if (actor.is_party_member || actor.is_local_player || actor.is_pet) {
            return true;
        }
        if (actor.actor_type == ActorType::Player || actor.actor_type == ActorType::Pet) {
            return true;
        }
        if (actor.actor_type == ActorType::Monster) {
            return false;
        }
        if (actor.role != Role::None) {
            return true;
        }
    }

    if (hub::game::is_monster_entity_id(entity_id)) {
        return false;
    }

    return true;
}

void CombatantRegistry::set_local_player(EntityId entity_id) {
    m_local_player_id = entity_id;
    for (auto& [id, actor] : m_actors) {
        actor.is_local_player = (id == entity_id);
    }
}

void CombatantRegistry::update_hp(EntityId entity_id, uint32_t current_hp, uint32_t max_hp) {
    auto it = m_actors.find(entity_id);
    if (it != m_actors.end()) {
        it->second.current_hp = current_hp;
        if (max_hp > 0) {
            it->second.max_hp = max_hp;
        }
    }
}

bool CombatantRegistry::is_party_wiped() const {
    // Dead means an HP reading of 0 out of a known max. An actor whose HP was never
    // read (max_hp 0, e.g. a party slot still loading) proves nothing either way.
    const auto confirmed_dead = [](const Combatant& actor) {
        return actor.max_hp > 0 && actor.current_hp == 0;
    };

    if (!m_party_members.empty()) {
        size_t party_dead = 0;

        for (EntityId id : m_party_members) {
            auto it = m_actors.find(id);
            if (it != m_actors.end() && confirmed_dead(it->second)) {
                party_dead++;
            }
        }

        // All synced party members must be confirmed dead
        return party_dead == m_party_members.size();
    }

    // Solo the party is the local player alone. Former party members, strangers
    // and pets are never read again, and one left alive blocked every solo wipe.
    if (m_local_player_id != 0) {
        const auto it = m_actors.find(m_local_player_id);
        return it != m_actors.end() && confirmed_dead(it->second);
    }

    // Fallback: check all players if no party was explicitly synced
    size_t players_known = 0;
    size_t players_dead = 0;

    for (const auto& [id, actor] : m_actors) {
        if (is_pet(id)) continue;
        if (actor.actor_type == ActorType::Player || actor.role != Role::None) {
            players_known++;
            if (confirmed_dead(actor)) {
                players_dead++;
            }
        }
    }

    return (players_known > 0) && (players_dead == players_known);
}

void CombatantRegistry::apply_status_list(const ipc::StatusListPacket& packet, std::vector<StatusChange>& changes) {
    if (!hub::game::is_real_entity_id(packet.entity_id)) {
        return;
    }
    const size_t count = std::min<size_t>(packet.count, ipc::MAX_STATUS_LIST_ENTRIES);
    Combatant* existing = find_actor_mut(packet.entity_id);
    if (existing == nullptr && count == 0) {
        return;
    }
    Combatant& actor = existing != nullptr ? *existing : get_or_create(packet.entity_id);

    const uint64_t ts = packet.timestamp_us;
    const bool detail = (packet.flags & ipc::STATUS_LIST_NO_DETAIL) == 0;
    const auto source_of = [detail](const ipc::CombatStatusEntry& entry) -> EntityId {
        return (detail && hub::game::is_real_entity_id(entry.source_id)) ? entry.source_id : 0;
    };
    const auto expected_end = [detail, ts](const ipc::CombatStatusEntry& entry) -> uint64_t {
        return (detail && entry.remaining_s > 0.0f)
            ? ts + static_cast<uint64_t>(static_cast<double>(entry.remaining_s) * 1e6)
            : 0;
    };

    const std::vector<ActiveStatus>& current = actor.statuses;
    std::vector<bool> taken(current.size(), false);
    int match[ipc::MAX_STATUS_LIST_ENTRIES];
    std::fill(std::begin(match), std::end(match), -1);

    // Same status from the same source first; then an unknown source on either side
    // (a list read without detail) still continues the status rather than restarting it.
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < count; ++i) {
            const ipc::CombatStatusEntry& entry = packet.entries[i];
            if (entry.status_id == 0 || match[i] >= 0) continue;
            const EntityId source = source_of(entry);
            for (size_t j = 0; j < current.size(); ++j) {
                if (taken[j] || current[j].status_id != entry.status_id) continue;
                const bool same = pass == 0
                    ? current[j].source == source
                    : (source == 0 || current[j].source == 0);
                if (same) {
                    taken[j] = true;
                    match[i] = static_cast<int>(j);
                    break;
                }
            }
        }
    }

    std::vector<ActiveStatus> next;
    next.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const ipc::CombatStatusEntry& entry = packet.entries[i];
        if (entry.status_id == 0) continue;
        if (match[i] >= 0) {
            ActiveStatus kept = current[static_cast<size_t>(match[i])];
            kept.param = entry.param;
            kept.expected_end_us = expected_end(entry);
            next.push_back(kept);
            continue;
        }
        const EntityId source = source_of(entry);
        next.push_back(ActiveStatus{entry.status_id, entry.param, source, ts, expected_end(entry)});
        changes.push_back(StatusChange{packet.entity_id, entry.status_id, source, ts, true});
    }

    for (size_t j = 0; j < current.size(); ++j) {
        if (taken[j]) continue;
        const ActiveStatus& gone = current[j];
        uint64_t at = ts;
        if (gone.expected_end_us != 0 && gone.expected_end_us < at) {
            at = gone.expected_end_us;
        }
        at = std::max({at, actor.statuses_seen_us, gone.since_us});
        changes.push_back(StatusChange{packet.entity_id, gone.status_id, gone.source, std::min(at, ts), false});
    }

    actor.statuses = std::move(next);
    actor.statuses_seen_us = ts;
}

void CombatantRegistry::clear_enemy_statuses(uint64_t at_us, std::vector<StatusChange>& changes) {
    for (auto& [id, actor] : m_actors) {
        if (actor.statuses.empty() || is_friendly(id)) continue;
        for (const ActiveStatus& gone : actor.statuses) {
            changes.push_back(StatusChange{id, gone.status_id, gone.source, std::max(at_us, gone.since_us), false});
        }
        actor.statuses.clear();
    }
}

void CombatantRegistry::add_label(std::vector<ActorLabel>& names, EntityId id) const {
    if (id == 0) return;
    for (const ActorLabel& label : names) {
        if (label.entity == id) return;
    }
    if (const Combatant* actor = find_actor(id)) {
        names.push_back(ActorLabel{id, actor->name, actor->job});
    }
}

void CombatantRegistry::clear() {
    m_actors.clear();
    m_pet_to_owner.clear();
    m_party_members.clear();
    m_local_player_id = 0;
}

} // namespace hub::meter
