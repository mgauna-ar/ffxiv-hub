#pragma once

#include "meter/types.hpp"
#include "meter/combatant_registry.hpp"
#include "meter/metrics_accumulator.hpp"
#include <cstdint>
#include <unordered_map>

namespace hub::meter {

/// Which enemy a pull is fought against, and how much HP it has left.
///
/// The boss is the enemy the pull damaged with the most max HP, the more damaged one
/// on a tie. HP comes only from the vitals pass's reads of the tracked enemies. Max HP
/// falls back to the registry, so with vitals off a pull still names its boss but
/// knows no HP for it.
class BossTracker {
public:
    /// One HP read of an enemy. A read of an unknown max proves nothing and is dropped.
    void observe_hp(EntityId enemy, uint32_t hp, uint32_t max_hp);

    /// The boss as things stand; id 0 when no enemy took damage.
    [[nodiscard]] BossSummary boss(const MetricsAccumulator& accumulator, const CombatantRegistry& registry) const;

    void clear() noexcept { m_readings.clear(); }

private:
    struct Reading {
        uint32_t hp{0};
        uint32_t max_hp{0};
    };
    std::unordered_map<EntityId, Reading> m_readings;
};

} // namespace hub::meter
