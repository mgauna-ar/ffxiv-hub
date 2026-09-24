#include "meter/metrics_accumulator.hpp"
#include "hub/game/entity.hpp"
#include "hub/game/limit_break.hpp"
#include <algorithm>

namespace hub::meter {

namespace {

void add_hit_counts(HitCounts& to, const HitCounts& from) {
    to.total_hits += from.total_hits;
    to.normal_hits += from.normal_hits;
    to.crit_hits += from.crit_hits;
    to.dh_hits += from.dh_hits;
    to.cdh_hits += from.cdh_hits;
    to.miss_hits += from.miss_hits;
    to.blocked_hits += from.blocked_hits;
    to.parried_hits += from.parried_hits;
    to.tick_hits += from.tick_hits;
}

} // namespace

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
    } else if (entity_id == hub::game::LIMIT_BREAK_COMBATANT_ID) {
        stats.name = "Limit Break";
        stats.actor_type = ActorType::LimitBreak;
    } else {
        stats.name = "Entity_" + std::to_string(entity_id);
        stats.actor_type = hub::game::is_monster_entity_id(entity_id) ? ActorType::Monster : ActorType::Player;
    }

    return m_combatants.emplace(entity_id, std::move(stats)).first->second;
}

void MetricsAccumulator::record_damage_hit(
    CombatantStats& stats,
    const ipc::CombatActionPacket& packet,
    HitSeverity severity,
    bool is_pet_hit,
    bool source_friendly,
    const CombatantRegistry& registry
) {
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

    // An effect that hit nothing carries the placeholder id, which would otherwise
    // open a junk combatant row.
    const EntityId target_id = static_cast<EntityId>(packet.target_id);
    if (hub::game::is_real_entity_id(target_id)) {
        CombatantStats& target_stats = get_or_create_stats(target_id, registry);
        target_stats.damage_taken += packet.damage;
        if (packet.damage > 0 && is_friendly_target(target_id, registry)) {
            record_taken(target_id, RecapSample{
                packet.timestamp_us, static_cast<EntityId>(packet.source_id), packet.action_id,
                packet.damage, RecapKind::Damage, static_cast<uint8_t>(packet.hit_flags)}, true);
        }
    }
}

void MetricsAccumulator::record_action(const ipc::CombatActionPacket& packet, const CombatantRegistry& registry) {
    const EntityId raw_source_id = static_cast<EntityId>(packet.source_id);
    if (!hub::game::is_real_entity_id(raw_source_id)) {
        return;
    }

    const EntityId resolved_owner = registry.resolve_owner(raw_source_id);

    // The game reports the casting player as the source of a Limit Break, so only the
    // action id distinguishes one. It belongs to the party rather than to whoever
    // pressed the button, and gets its own row instead of inflating that player's
    // personal DPS and share. The action list also carries duty-action and NPC limit
    // breaks, so an enemy casting one stays an enemy hit.
    const bool is_limit_break = hub::game::is_limit_break_action(packet.action_id)
        && registry.is_friendly(resolved_owner);

    const EntityId owner_id = is_limit_break
        ? static_cast<EntityId>(hub::game::LIMIT_BREAK_COMBATANT_ID)
        : resolved_owner;
    const bool is_pet_hit = !is_limit_break
        && ((raw_source_id != owner_id) || registry.is_pet(raw_source_id));
    const bool source_friendly = is_limit_break || registry.is_friendly(owner_id);

    CombatantStats& stats = get_or_create_stats(owner_id, registry);

    HitSeverity severity = static_cast<HitSeverity>(packet.severity);
    if (severity == HitSeverity::Normal && packet.hit_flags != 0) {
        severity = hit_flags_to_severity(packet.hit_flags);
    }

    const EffectType effect = static_cast<EffectType>(packet.effect_type);

    if (effect == EffectType::Damage) {
        record_damage_hit(stats, packet, severity, is_pet_hit, source_friendly, registry);
    } else if (effect == EffectType::Blocked) {
        // Partially mitigated, but the value that survived is still damage dealt.
        record_damage_hit(stats, packet, severity, is_pet_hit, source_friendly, registry);
        stats.hits.blocked_hits++;
        stats.actions[packet.action_id].hits.blocked_hits++;
    } else if (effect == EffectType::Parried) {
        record_damage_hit(stats, packet, severity, is_pet_hit, source_friendly, registry);
        stats.hits.parried_hits++;
        stats.actions[packet.action_id].hits.parried_hits++;
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

        const EntityId target_id = static_cast<EntityId>(packet.target_id);
        if (eff_heal > 0 && is_friendly_target(target_id, registry)) {
            record_taken(target_id, RecapSample{
                packet.timestamp_us, raw_source_id, packet.action_id,
                eff_heal, RecapKind::Heal, static_cast<uint8_t>(packet.hit_flags)}, false);
        }

        if (source_friendly) {
            m_total_healing += raw_heal;
            m_total_effective_healing += eff_heal;
            m_total_overhealing += over_heal;
        }

        const bool heal_crit = (severity == HitSeverity::Critical || severity == HitSeverity::CritDirectHit);
        stats.heal_hit_counts.total_hits++;
        if (heal_crit) {
            stats.heal_hit_counts.crit_hits++;
        } else {
            stats.heal_hit_counts.normal_hits++;
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
        act.heal_hit_counts.total_hits++;
        if (heal_crit) {
            act.heal_hit_counts.crit_hits++;
        } else {
            act.heal_hit_counts.normal_hits++;
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
    }
}

void MetricsAccumulator::record_status_tick(const ipc::StatusTickPacket& packet, const CombatantRegistry& registry) {
    if (packet.damage_or_heal == 0) {
        return;
    }
    const EntityId raw_source_id = packet.source_id;
    if (!hub::game::is_real_entity_id(raw_source_id)) {
        return;
    }

    const EntityId owner_id = registry.resolve_owner(raw_source_id);
    const bool is_pet_hit = (raw_source_id != owner_id) || registry.is_pet(raw_source_id);
    const bool source_friendly = registry.is_friendly(owner_id);

    CombatantStats& stats = get_or_create_stats(owner_id, registry);
    const EffectType effect = static_cast<EffectType>(packet.effect_type);

    // Ticks carry no severity: ProcessHotDot reports no crit flag, so they are counted
    // separately rather than booked as guaranteed non-crits.
    const ActionId action_key = packet.status_id | STATUS_ACTION_KEY_OFFSET;

    if (effect == EffectType::Damage) {
        stats.total_damage += packet.damage_or_heal;
        if (is_pet_hit) {
            stats.pet_damage += packet.damage_or_heal;
        }
        if (source_friendly) {
            m_total_damage += packet.damage_or_heal;
        }

        stats.hits.tick_hits++;

        ActionSummary& act = stats.actions[action_key];
        act.action_id = packet.status_id;
        act.is_status = true;
        if (act.name.empty()) {
            act.name = status_id_to_name(packet.status_id);
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
        act.hits.tick_hits++;

        if (hub::game::is_real_entity_id(packet.target_id)) {
            CombatantStats& target_stats = get_or_create_stats(packet.target_id, registry);
            target_stats.damage_taken += packet.damage_or_heal;
            if (is_friendly_target(packet.target_id, registry)) {
                record_taken(packet.target_id, RecapSample{
                    packet.timestamp_us, raw_source_id, action_key,
                    packet.damage_or_heal, RecapKind::DotTick, 0}, true);
            }
        }
    } else if (effect == EffectType::Heal) {
        // Split in-game like a direct heal. Min, max and the recap use what landed.
        const uint32_t over_heal = std::min(packet.overheal, packet.damage_or_heal);
        const uint32_t eff_heal = packet.damage_or_heal - over_heal;

        stats.total_healing += packet.damage_or_heal;
        stats.effective_healing += eff_heal;
        stats.overhealing += over_heal;
        if (source_friendly) {
            m_total_healing += packet.damage_or_heal;
            m_total_effective_healing += eff_heal;
            m_total_overhealing += over_heal;
        }

        stats.heal_hit_counts.tick_hits++;

        ActionSummary& act = stats.actions[action_key];
        act.action_id = packet.status_id;
        act.is_status = true;
        if (act.name.empty()) {
            act.name = status_id_to_name(packet.status_id);
        }
        act.hit_count++;
        act.heal_hits++;
        act.total_healing += packet.damage_or_heal;
        act.effective_healing += eff_heal;
        act.overhealing += over_heal;
        if (act.heal_hits == 1 || eff_heal < act.min_heal) {
            act.min_heal = eff_heal;
        }
        if (eff_heal > act.max_heal) {
            act.max_heal = eff_heal;
        }
        act.heal_hit_counts.tick_hits++;

        if (eff_heal > 0 && is_friendly_target(packet.target_id, registry)) {
            record_taken(packet.target_id, RecapSample{
                packet.timestamp_us, raw_source_id, action_key,
                eff_heal, RecapKind::HotTick, 0}, false);
        }
    }
}

bool MetricsAccumulator::is_friendly_target(EntityId target_id, const CombatantRegistry& registry) {
    return hub::game::is_real_entity_id(target_id)
        && target_id != hub::game::LIMIT_BREAK_COMBATANT_ID
        && registry.is_friendly(target_id)
        && !registry.is_pet(target_id);
}

void MetricsAccumulator::record_taken(EntityId target_id, const RecapSample& sample, bool is_damage) {
    m_recaps[target_id].push(sample);
    if (!is_damage) {
        return;
    }
    DamageTakenRow& row = m_damage_taken[DamageTakenKey{target_id, sample.action_key, sample.source}];
    row.target = target_id;
    row.action_key = sample.action_key;
    row.source = sample.source;
    row.hits++;
    row.total += sample.amount;
    row.max = std::max<uint64_t>(row.max, sample.amount);
}

std::vector<DamageTakenRow> MetricsAccumulator::damage_taken_rows() const {
    std::vector<DamageTakenRow> rows;
    rows.reserve(m_damage_taken.size());
    for (const auto& [key, row] : m_damage_taken) {
        rows.push_back(row);
    }
    std::sort(rows.begin(), rows.end(), [](const DamageTakenRow& a, const DamageTakenRow& b) {
        if (a.total != b.total) return a.total > b.total;
        if (a.target != b.target) return a.target < b.target;
        if (a.action_key != b.action_key) return a.action_key < b.action_key;
        return a.source < b.source;
    });
    return rows;
}

void MetricsAccumulator::add_death_caused(EntityId target, ActionId action_key, EntityId source) {
    auto it = m_damage_taken.find(DamageTakenKey{target, action_key, source});
    if (it != m_damage_taken.end()) {
        it->second.deaths_caused++;
    }
}

const RecapRing* MetricsAccumulator::recap_for(EntityId target) const {
    auto it = m_recaps.find(target);
    return it != m_recaps.end() ? &it->second : nullptr;
}

std::vector<EntityId> MetricsAccumulator::top_enemies(size_t max, const CombatantRegistry& registry) const {
    std::vector<std::pair<uint64_t, EntityId>> enemies;
    for (const auto& [id, stats] : m_combatants) {
        if (stats.damage_taken == 0 || !hub::game::is_real_entity_id(id)) continue;
        if (id == hub::game::LIMIT_BREAK_COMBATANT_ID || registry.is_friendly(id)) continue;
        enemies.emplace_back(stats.damage_taken, id);
    }
    std::sort(enemies.begin(), enemies.end(), [](const auto& a, const auto& b) {
        return a.first != b.first ? a.first > b.first : a.second < b.second;
    });
    std::vector<EntityId> ids;
    for (size_t i = 0; i < enemies.size() && i < max; ++i) {
        ids.push_back(enemies[i].second);
    }
    return ids;
}

void MetricsAccumulator::merge_combatants(EntityId from_id, EntityId to_id, const CombatantRegistry& registry) {
    if (from_id == 0 || to_id == 0 || from_id == to_id) {
        return;
    }
    auto it_from = m_combatants.find(from_id);
    if (it_from == m_combatants.end()) {
        return;
    }

    CombatantStats from_stats = std::move(it_from->second);
    m_combatants.erase(it_from);

    // An owner with no row yet gets one built from its own registry entry. Moving
    // the pet's row across kept the pet's name and type, and neither metadata
    // refresh ever replaces a name that is not a placeholder.
    CombatantStats& to = get_or_create_stats(to_id, registry);
    to.total_damage += from_stats.total_damage;
    // All of it is pet damage from the owner's side. The merged entry's own
    // pet_damage is already inside its total_damage, so adding it again doubles it.
    to.pet_damage += from_stats.total_damage;
    to.damage_taken += from_stats.damage_taken;
    to.total_healing += from_stats.total_healing;
    to.effective_healing += from_stats.effective_healing;
    to.overhealing += from_stats.overhealing;
    to.deaths += from_stats.deaths;
    to.raises += from_stats.raises;

    add_hit_counts(to.hits, from_stats.hits);
    add_hit_counts(to.heal_hit_counts, from_stats.heal_hit_counts);

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

            add_hit_counts(to_act.hits, from_act.hits);
            add_hit_counts(to_act.heal_hit_counts, from_act.heal_hit_counts);
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
            merge_combatants(pet_id, owner_id, *registry);
        }
    }

    const double safe_duration = std::max(duration_seconds, 1.0);

    m_total_dps = static_cast<double>(m_total_damage) / safe_duration;
    m_total_hps = static_cast<double>(m_total_effective_healing) / safe_duration;

    for (auto& [id, stats] : m_combatants) {
        stats.dps = static_cast<double>(stats.total_damage) / safe_duration;
        stats.hps = static_cast<double>(stats.effective_healing) / safe_duration;
        // m_total_damage counts friendly sources only, so an enemy row measured against
        // it would report a share of a total it never contributed to.
        stats.damage_share_pct = (m_total_damage > 0 && stats.is_friendly())
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

std::vector<CombatantStats> MetricsAccumulator::sorted_by_dps(bool friendly_only, bool with_actions) const {
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
        if (with_actions) {
            list.push_back(stats);
        } else {
            CombatantStats totals;
            static_cast<CombatantTotals&>(totals) = stats;
            list.push_back(std::move(totals));
        }
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
    m_damage_taken.clear();
    m_recaps.clear();
    m_total_damage = 0;
    m_total_healing = 0;
    m_total_effective_healing = 0;
    m_total_overhealing = 0;
    m_total_dps = 0.0;
    m_total_hps = 0.0;
}

} // namespace hub::meter
