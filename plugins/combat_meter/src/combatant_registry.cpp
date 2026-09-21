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
    std::string name(name_buf);
    if (name.empty()) {
        name = "Actor_" + std::to_string(packet.entity_id);
    }

    Job job = static_cast<Job>(packet.job_id);
    const bool pet_by_name = is_known_pet_name(name);
    const bool is_pet_actor = (packet.owner_id != 0) || pet_by_name;

    if (job == Job::None && is_pet_actor) {
        job = infer_pet_job(name);
    }

    ActorType actor_type = static_cast<ActorType>(packet.actor_type);
    if (actor_type == ActorType::Unknown) {
        if (is_pet_actor) {
            actor_type = ActorType::Pet;
        } else if (job_to_role(job) != Role::None) {
            actor_type = ActorType::Player;
        }
    }

    EntityId resolved_owner_id = packet.owner_id;
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

    Combatant& actor = m_actors[packet.entity_id];
    actor.entity_id = packet.entity_id;
    actor.owner_id = resolved_owner_id;
    actor.name = std::move(name);
    actor.job = job;
    actor.role = job_to_role(job);
    actor.actor_type = actor_type;
    actor.max_hp = packet.max_hp;
    actor.current_hp = packet.current_hp;
    actor.world_id = packet.world_id;
    actor.is_pet = is_pet_actor;
    actor.is_party_member = is_party_member(packet.entity_id);
    actor.is_local_player = (packet.entity_id == m_local_player_id);

    if (resolved_owner_id != 0) {
        m_pet_to_owner[packet.entity_id] = resolved_owner_id;
    }
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
    actor.current_hp = (current_hp > 0) ? current_hp : max_hp;
    actor.is_pet = is_pet_actor;
    actor.is_party_member = is_party_member(entity_id);
    actor.is_local_player = (entity_id == m_local_player_id);

    if (resolved_owner_id != 0) {
        m_pet_to_owner[entity_id] = resolved_owner_id;
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
    const uint32_t count = std::min(packet.party_count, static_cast<uint32_t>(constants::MAX_PARTY_MEMBERS));

    for (uint32_t i = 0; i < count; ++i) {
        const EntityId id = packet.entity_ids[i];
        if (id == 0) continue;

        m_party_members.push_back(id);
        auto it = m_actors.find(id);
        if (it != m_actors.end()) {
            it->second.is_party_member = true;
            if (it->second.job == Job::None && packet.job_ids[i] != 0) {
                it->second.job = static_cast<Job>(packet.job_ids[i]);
                it->second.role = job_to_role(it->second.job);
            }
        } else if (packet.job_ids[i] != 0) {
            Combatant& new_actor = get_or_create(id);
            new_actor.is_party_member = true;
            new_actor.job = static_cast<Job>(packet.job_ids[i]);
            new_actor.role = job_to_role(new_actor.job);
        }
    }

    if (count > 0 && packet.entity_ids[0] != 0 && m_local_player_id == 0) {
        m_local_player_id = packet.entity_ids[0];
        auto lp = m_actors.find(m_local_player_id);
        if (lp != m_actors.end()) {
            lp->second.is_local_player = true;
        }
    }
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
    if (!m_party_members.empty()) {
        size_t party_dead = 0;

        for (EntityId id : m_party_members) {
            auto it = m_actors.find(id);
            if (it != m_actors.end() && it->second.current_hp == 0) {
                party_dead++;
            }
        }

        // All synced party members must be confirmed dead
        return party_dead == m_party_members.size();
    }

    // Fallback: check all players if no party was explicitly synced
    size_t players_known = 0;
    size_t players_dead = 0;

    for (const auto& [id, actor] : m_actors) {
        if (actor.actor_type == ActorType::Player || actor.role != Role::None) {
            players_known++;
            if (actor.current_hp == 0) {
                players_dead++;
            }
        }
    }

    return (players_known > 0) && (players_dead == players_known);
}

void CombatantRegistry::clear() {
    m_actors.clear();
    m_pet_to_owner.clear();
    m_party_members.clear();
    m_local_player_id = 0;
}

} // namespace hub::meter
