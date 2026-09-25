#include "meter/boss_tracker.hpp"

namespace hub::meter {

void BossTracker::observe_hp(EntityId enemy, uint32_t hp, uint32_t max_hp) {
    if (max_hp == 0) {
        return;
    }
    m_readings[enemy] = Reading{hp, max_hp};
}

BossSummary BossTracker::boss(const MetricsAccumulator& accumulator, const CombatantRegistry& registry) const {
    EntityId best_id = 0;
    uint32_t best_max_hp = 0;
    uint64_t best_damage = 0;
    for (const auto& [id, stats] : accumulator.combatants()) {
        if (stats.damage_taken == 0 || !MetricsAccumulator::is_enemy(id, registry)) continue;
        uint32_t max_hp = 0;
        if (const auto it = m_readings.find(id); it != m_readings.end()) {
            max_hp = it->second.max_hp;
        } else if (const Combatant* actor = registry.find_actor(id)) {
            max_hp = actor->max_hp;
        }
        const bool better = best_id == 0 || max_hp > best_max_hp
            || (max_hp == best_max_hp && (stats.damage_taken > best_damage
                                          || (stats.damage_taken == best_damage && id < best_id)));
        if (better) {
            best_id = id;
            best_max_hp = max_hp;
            best_damage = stats.damage_taken;
        }
    }

    BossSummary boss;
    if (best_id == 0) {
        return boss;
    }
    boss.id = best_id;
    if (const Combatant* actor = registry.find_actor(best_id)) {
        boss.name = actor->name;
    }
    if (const auto it = m_readings.find(best_id); it != m_readings.end()) {
        boss.hp_pct = 100.0 * static_cast<double>(it->second.hp) / static_cast<double>(it->second.max_hp);
        boss.killed = it->second.hp == 0;
    }
    return boss;
}

} // namespace hub::meter
