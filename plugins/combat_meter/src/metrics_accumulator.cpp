#include "meter/metrics_accumulator.hpp"
#include "hub/game/entity.hpp"
#include <algorithm>

namespace hub::meter {

CombatantStats& MetricsAccumulator::get_or_create_stats(EntityId entity_id, const CombatantRegistry& registry) {
    auto it = m_combatants.find(entity_id);
    if (it != m_combatants.end()) {
        if (it->second.actor_type == ActorType::Unknown || it->second.job == Job::None) {
            const Combatant* actor = registry.find_actor(entity_id);
            if (actor != nullptr) {
                if (!actor->name.empty()) it->second.name = actor->name;
                if (actor->job != Job::None) it->second.job = actor->job;
                if (actor->role != Role::None) it->second.role = actor->role;
                it->second.owner_id = actor->owner_id;
                it->second.actor_type = actor->actor_type;
                it->second.is_pet = actor->is_pet;
                it->second.is_party_member = actor->is_party_member;
                it->second.is_local_player = actor->is_local_player;
                it->second.is_alive = (actor->current_hp > 0 || actor->max_hp == 0);
            }
        }
        return it->second;
    }

    CombatantStats stats;
    stats.entity_id = entity_id;

    const Combatant* actor = registry.find_actor(entity_id);
    if (actor != nullptr) {
        stats.name = actor->name;
        stats.job = actor->job;
        stats.role = actor->role;
        stats.owner_id = actor->owner_id;
        stats.actor_type = actor->actor_type;
        stats.is_pet = actor->is_pet;
        stats.is_party_member = actor->is_party_member;
        stats.is_local_player = actor->is_local_player;
        stats.is_alive = (actor->current_hp > 0 || actor->max_hp == 0);
    } else {
        stats.name = "Entity_" + std::to_string(entity_id);
        stats.actor_type = hub::game::is_monster_entity_id(entity_id) ? ActorType::Monster : ActorType::Player;
    }

    return m_combatants.emplace(entity_id, std::move(stats)).first->second;
}

void MetricsAccumulator::record_action(const ipc::CombatActionPacket& packet, const CombatantRegistry& registry) {
    const EntityId raw_source_id = static_cast<EntityId>(packet.source_id);
    if (raw_source_id == 0) {
        return;
    }

    const EntityId owner_id = registry.resolve_owner(raw_source_id);
    const bool is_pet_hit = (raw_source_id != owner_id) || registry.is_pet(raw_source_id);
    const bool source_friendly = registry.is_friendly(owner_id);

    CombatantStats& stats = get_or_create_stats(owner_id, registry);

    HitSeverity severity = static_cast<HitSeverity>(packet.severity);
    if (severity == HitSeverity::Normal && packet.hit_flags != 0) {
        severity = hit_flags_to_severity(packet.hit_flags);
    }

    const EffectType effect = static_cast<EffectType>(packet.effect_type);

    if (effect == EffectType::Damage) {
        stats.total_damage += packet.damage;
        if (is_pet_hit) {
            stats.pet_damage += packet.damage;
        }
        if (source_friendly) {
            m_total_damage += packet.damage;
        }

        stats.hits.total_hits++;
        switch (severity) {
            case HitSeverity::Normal: stats.hits.normal_hits++; break;
            case HitSeverity::Critical: stats.hits.crit_hits++; break;
            case HitSeverity::DirectHit: stats.hits.dh_hits++; break;
            case HitSeverity::CritDirectHit: stats.hits.cdh_hits++; break;
        }

        ActionSummary& act = stats.actions[packet.action_id];
        act.action_id = packet.action_id;
        if (act.name.empty()) {
            act.name = action_id_to_name(packet.action_id);
        }
        act.hit_count++;
        act.damage_hits++;
        act.total_damage += packet.damage;
        if (act.damage_hits == 1 || packet.damage < act.min_damage) {
            act.min_damage = packet.damage;
        }
        if (packet.damage > act.max_damage) {
            act.max_damage = packet.damage;
        }
        act.hits.total_hits++;
        switch (severity) {
            case HitSeverity::Normal: act.hits.normal_hits++; break;
            case HitSeverity::Critical: act.hits.crit_hits++; break;
            case HitSeverity::DirectHit: act.hits.dh_hits++; break;
            case HitSeverity::CritDirectHit: act.hits.cdh_hits++; break;
        }

        const EntityId target_id = static_cast<EntityId>(packet.target_id);
        if (target_id != 0) {
            CombatantStats& target_stats = get_or_create_stats(target_id, registry);
            target_stats.damage_taken += packet.damage;
        }
    } else if (effect == EffectType::Heal) {
        uint32_t raw_heal = packet.damage;
        uint32_t eff_heal = packet.effective_heal;
        uint32_t over_heal = packet.overheal;

        if (raw_heal == 0 && (eff_heal > 0 || over_heal > 0)) {
            raw_heal = eff_heal + over_heal;
        } else if (eff_heal == 0 && over_heal == 0 && raw_heal > 0) {
            eff_heal = raw_heal;
        } else if (over_heal == 0 && eff_heal > 0 && raw_heal > eff_heal) {
            over_heal = raw_heal - eff_heal;
        } else if (eff_heal == 0 && over_heal > 0 && raw_heal > over_heal) {
            eff_heal = raw_heal - over_heal;
        }

        stats.total_healing += raw_heal;
        stats.effective_healing += eff_heal;
        stats.overhealing += over_heal;

        if (source_friendly) {
            m_total_healing += raw_heal;
            m_total_effective_healing += eff_heal;
            m_total_overhealing += over_heal;
        }

        stats.hits.total_hits++;
        switch (severity) {
            case HitSeverity::Critical:
            case HitSeverity::CritDirectHit:
                stats.hits.crit_hits++;
                break;
            default:
                stats.hits.normal_hits++;
                break;
        }

        ActionSummary& act = stats.actions[packet.action_id];
        act.action_id = packet.action_id;
        if (act.name.empty()) {
            act.name = action_id_to_name(packet.action_id);
        }
        act.hit_count++;
        act.heal_hits++;
        act.total_healing += raw_heal;
        act.effective_healing += eff_heal;
        act.overhealing += over_heal;
        if (act.heal_hits == 1 || eff_heal < act.min_heal) {
            act.min_heal = eff_heal;
        }
        if (eff_heal > act.max_heal) {
            act.max_heal = eff_heal;
        }
        act.hits.total_hits++;
        switch (severity) {
            case HitSeverity::Critical:
            case HitSeverity::CritDirectHit:
                act.hits.crit_hits++;
                break;
            default:
                act.hits.normal_hits++;
                break;
        }
    } else if (effect == EffectType::Miss) {
        stats.hits.total_hits++;
        stats.hits.miss_hits++;
        ActionSummary& act = stats.actions[packet.action_id];
        act.action_id = packet.action_id;
        if (act.name.empty()) {
            act.name = action_id_to_name(packet.action_id);
        }
        act.hit_count++;
        act.hits.total_hits++;
        act.hits.miss_hits++;
    } else if (effect == EffectType::Blocked) {
        stats.hits.total_hits++;
        stats.hits.blocked_hits++;
        ActionSummary& act = stats.actions[packet.action_id];
        act.action_id = packet.action_id;
        if (act.name.empty()) {
            act.name = action_id_to_name(packet.action_id);
        }
        act.hit_count++;
        act.hits.total_hits++;
        act.hits.blocked_hits++;
    } else if (effect == EffectType::Parried) {
        stats.hits.total_hits++;
        stats.hits.parried_hits++;
        ActionSummary& act = stats.actions[packet.action_id];
        act.action_id = packet.action_id;
        if (act.name.empty()) {
            act.name = action_id_to_name(packet.action_id);
        }
        act.hit_count++;
        act.hits.total_hits++;
        act.hits.parried_hits++;
    }
}

void MetricsAccumulator::record_status_tick(const ipc::StatusTickPacket& packet, const CombatantRegistry& registry) {
    if (packet.damage_or_heal == 0) {
        return;
    }
    const EntityId raw_source_id = packet.source_id;
    if (raw_source_id == 0) {
        return;
    }

    const EntityId owner_id = registry.resolve_owner(raw_source_id);
    const bool is_pet_hit = (raw_source_id != owner_id) || registry.is_pet(raw_source_id);
    const bool source_friendly = registry.is_friendly(owner_id);

    CombatantStats& stats = get_or_create_stats(owner_id, registry);
    const EffectType effect = static_cast<EffectType>(packet.effect_type);

    if (effect == EffectType::Damage || effect == EffectType::None) {
        stats.total_damage += packet.damage_or_heal;
        if (is_pet_hit) {
            stats.pet_damage += packet.damage_or_heal;
        }
        if (source_friendly) {
            m_total_damage += packet.damage_or_heal;
        }

        stats.hits.total_hits++;
        if (packet.is_crit != 0) {
            stats.hits.crit_hits++;
        } else {
            stats.hits.normal_hits++;
        }

        ActionSummary& act = stats.actions[packet.status_id];
        act.action_id = packet.status_id;
        if (act.name.empty()) {
            act.name = action_id_to_name(packet.status_id);
        }
        act.hit_count++;
        act.damage_hits++;
        act.total_damage += packet.damage_or_heal;
        if (act.damage_hits == 1 || packet.damage_or_heal < act.min_damage) {
            act.min_damage = packet.damage_or_heal;
        }
        if (packet.damage_or_heal > act.max_damage) {
            act.max_damage = packet.damage_or_heal;
        }
        act.hits.total_hits++;
        if (packet.is_crit != 0) {
            act.hits.crit_hits++;
        } else {
            act.hits.normal_hits++;
        }

        if (packet.target_id != 0) {
            CombatantStats& target_stats = get_or_create_stats(packet.target_id, registry);
            target_stats.damage_taken += packet.damage_or_heal;
        }
    } else if (effect == EffectType::Heal) {
        stats.total_healing += packet.damage_or_heal;
        stats.effective_healing += packet.damage_or_heal;
        if (source_friendly) {
            m_total_healing += packet.damage_or_heal;
            m_total_effective_healing += packet.damage_or_heal;
        }

        stats.hits.total_hits++;
        if (packet.is_crit != 0) {
            stats.hits.crit_hits++;
        } else {
            stats.hits.normal_hits++;
        }

        ActionSummary& act = stats.actions[packet.status_id];
        act.action_id = packet.status_id;
        if (act.name.empty()) {
            act.name = action_id_to_name(packet.status_id);
        }
        act.hit_count++;
        act.heal_hits++;
        act.total_healing += packet.damage_or_heal;
        act.effective_healing += packet.damage_or_heal;
        if (act.heal_hits == 1 || packet.damage_or_heal < act.min_heal) {
            act.min_heal = packet.damage_or_heal;
        }
        if (packet.damage_or_heal > act.max_heal) {
            act.max_heal = packet.damage_or_heal;
        }
        act.hits.total_hits++;
        if (packet.is_crit != 0) {
            act.hits.crit_hits++;
        } else {
            act.hits.normal_hits++;
        }
    }
}

void MetricsAccumulator::merge_combatants(EntityId from_id, EntityId to_id) {
    if (from_id == 0 || to_id == 0 || from_id == to_id) {
        return;
    }
    auto it_from = m_combatants.find(from_id);
    if (it_from == m_combatants.end()) {
        return;
    }

    CombatantStats from_stats = std::move(it_from->second);
    m_combatants.erase(it_from);

    auto it_to = m_combatants.find(to_id);
    if (it_to == m_combatants.end()) {
        from_stats.entity_id = to_id;
        from_stats.owner_id = 0;
        from_stats.is_pet = false;
        m_combatants[to_id] = std::move(from_stats);
        return;
    }

    CombatantStats& to = it_to->second;
    to.total_damage += from_stats.total_damage;
    // Plus the merged entry's own pet share, or a pet-of-a-pet chain loses it.
    to.pet_damage += from_stats.total_damage + from_stats.pet_damage;
    to.damage_taken += from_stats.damage_taken;
    to.total_healing += from_stats.total_healing;
    to.effective_healing += from_stats.effective_healing;
    to.overhealing += from_stats.overhealing;

    to.hits.total_hits += from_stats.hits.total_hits;
    to.hits.normal_hits += from_stats.hits.normal_hits;
    to.hits.crit_hits += from_stats.hits.crit_hits;
    to.hits.dh_hits += from_stats.hits.dh_hits;
    to.hits.cdh_hits += from_stats.hits.cdh_hits;
    to.hits.miss_hits += from_stats.hits.miss_hits;
    to.hits.blocked_hits += from_stats.hits.blocked_hits;
    to.hits.parried_hits += from_stats.hits.parried_hits;

    for (const auto& [act_id, from_act] : from_stats.actions) {
        auto [it_act, inserted] = to.actions.try_emplace(act_id, from_act);
        if (!inserted) {
            ActionSummary& to_act = it_act->second;
            to_act.hit_count += from_act.hit_count;
            to_act.damage_hits += from_act.damage_hits;
            to_act.heal_hits += from_act.heal_hits;
            to_act.total_damage += from_act.total_damage;
            to_act.total_healing += from_act.total_healing;
            to_act.effective_healing += from_act.effective_healing;
            to_act.overhealing += from_act.overhealing;

            if (from_act.damage_hits > 0) {
                if (to_act.min_damage == 0 || from_act.min_damage < to_act.min_damage) {
                    to_act.min_damage = from_act.min_damage;
                }
                if (from_act.max_damage > to_act.max_damage) {
                    to_act.max_damage = from_act.max_damage;
                }
            }
            if (from_act.heal_hits > 0) {
                if (to_act.min_heal == 0 || from_act.min_heal < to_act.min_heal) {
                    to_act.min_heal = from_act.min_heal;
                }
                if (from_act.max_heal > to_act.max_heal) {
                    to_act.max_heal = from_act.max_heal;
                }
            }

            to_act.hits.total_hits += from_act.hits.total_hits;
            to_act.hits.normal_hits += from_act.hits.normal_hits;
            to_act.hits.crit_hits += from_act.hits.crit_hits;
            to_act.hits.dh_hits += from_act.hits.dh_hits;
            to_act.hits.cdh_hits += from_act.hits.cdh_hits;
            to_act.hits.miss_hits += from_act.hits.miss_hits;
            to_act.hits.blocked_hits += from_act.hits.blocked_hits;
            to_act.hits.parried_hits += from_act.hits.parried_hits;
        }
    }
}

void MetricsAccumulator::recalculate(double duration_seconds, const CombatantRegistry* registry) {
    if (registry != nullptr) {
        // Refresh combatant metadata (names, jobs, roles, party status) from registry if updated
        for (auto& [id, stats] : m_combatants) {
            const Combatant* actor = registry->find_actor(id);
            if (actor != nullptr) {
                if ((stats.name.empty() || stats.name.rfind("Actor_", 0) == 0 || stats.name.rfind("Entity_", 0) == 0) && !actor->name.empty()) {
                    stats.name = actor->name;
                }
                if (stats.job == Job::None && actor->job != Job::None) {
                    stats.job = actor->job;
                    stats.role = actor->role;
                }
                if (stats.actor_type == ActorType::Unknown && actor->actor_type != ActorType::Unknown) {
                    stats.actor_type = actor->actor_type;
                }
                stats.is_party_member = actor->is_party_member;
                stats.is_local_player = actor->is_local_player;
            }
        }

        std::vector<std::pair<EntityId, EntityId>> to_merge;
        for (const auto& [id, stats] : m_combatants) {
            const EntityId owner = registry->resolve_owner(id);
            if (owner != id && owner != 0) {
                to_merge.emplace_back(id, owner);
            }
        }
        for (const auto& [pet_id, owner_id] : to_merge) {
            merge_combatants(pet_id, owner_id);
        }
    }

    const double safe_duration = std::max(duration_seconds, 1.0);

    m_total_dps = static_cast<double>(m_total_damage) / safe_duration;
    m_total_hps = static_cast<double>(m_total_effective_healing) / safe_duration;

    for (auto& [id, stats] : m_combatants) {
        stats.dps = static_cast<double>(stats.total_damage) / safe_duration;
        stats.hps = static_cast<double>(stats.effective_healing) / safe_duration;
        stats.damage_share_pct = (m_total_damage > 0)
            ? (static_cast<double>(stats.total_damage) * 100.0 / static_cast<double>(m_total_damage))
            : 0.0;
    }
}

const CombatantStats* MetricsAccumulator::find_stats(EntityId entity_id) const {
    auto it = m_combatants.find(entity_id);
    if (it != m_combatants.end()) {
        return &it->second;
    }
    return nullptr;
}

std::vector<CombatantStats> MetricsAccumulator::sorted_by_dps(bool friendly_only) const {
    std::vector<CombatantStats> list;
    list.reserve(m_combatants.size());
    for (const auto& [id, stats] : m_combatants) {
        if (stats.is_pet) {
            continue;
        }
        if (friendly_only) {
            if (stats.actor_type == ActorType::Monster) {
                continue;
            }
            if (stats.total_damage == 0 && !stats.is_party_member && !stats.is_local_player) {
                continue;
            }
        }
        list.push_back(stats);
    }

    std::sort(list.begin(), list.end(), [](const CombatantStats& a, const CombatantStats& b) {
        if (a.dps != b.dps) {
            return a.dps > b.dps;
        }
        return a.total_damage > b.total_damage;
    });

    return list;
}

std::vector<CombatantStats> MetricsAccumulator::sorted_by_hps(bool friendly_only) const {
    std::vector<CombatantStats> list;
    list.reserve(m_combatants.size());
    for (const auto& [id, stats] : m_combatants) {
        if (stats.is_pet) {
            continue;
        }
        if (friendly_only) {
            if (stats.actor_type == ActorType::Monster) {
                continue;
            }
            if (stats.total_healing == 0 && !stats.is_party_member && !stats.is_local_player) {
                continue;
            }
        }
        list.push_back(stats);
    }

    std::sort(list.begin(), list.end(), [](const CombatantStats& a, const CombatantStats& b) {
        if (a.hps != b.hps) {
            return a.hps > b.hps;
        }
        return a.effective_healing > b.effective_healing;
    });

    return list;
}

double MetricsAccumulator::overheal_pct() const noexcept {
    const uint64_t all_heal = m_total_effective_healing + m_total_overhealing;
    return (all_heal > 0)
        ? (static_cast<double>(m_total_overhealing) / static_cast<double>(all_heal)) * 100.0
        : 0.0;
}

void MetricsAccumulator::clear() {
    m_combatants.clear();
    m_total_damage = 0;
    m_total_healing = 0;
    m_total_effective_healing = 0;
    m_total_overhealing = 0;
    m_total_dps = 0.0;
    m_total_hps = 0.0;
}

} // namespace hub::meter
