#pragma once

#include "meter/types.hpp"
#include "meter/combatant_registry.hpp"
#include "meter/metrics_accumulator.hpp"
#include "common/ipc/protocol.hpp"
#include <cstdint>
#include <vector>

namespace hub::meter {

/// Deaths and raises of one pull.
class DeathLog {
public:
    /// How far back a death recap reaches.
    static constexpr uint64_t kRecapWindowUs = 12'000'000;

    /// A life event for `entity`. A death carries the newest events that landed on
    /// it within kRecapWindowUs, oldest first.
    [[nodiscard]] static ipc::LifeEventPacket build_event(
        EntityId entity, LifeEventKind kind, uint64_t timestamp_us, const MetricsAccumulator& accumulator);

    /// Records into the live pull: the record, the combatant's counts and the
    /// killing blow's damage-taken row.
    void record(const ipc::LifeEventPacket& packet, uint64_t pull_start_us,
                const CombatantRegistry& registry, MetricsAccumulator& accumulator);

    /// Same, into a pull that has already been archived.
    static void record_into(EncounterSummary& pull, const ipc::LifeEventPacket& packet,
                            const CombatantRegistry& registry);

    [[nodiscard]] const std::vector<DeathRecord>& deaths() const noexcept { return m_deaths; }
    void clear() noexcept { m_deaths.clear(); }

private:
    std::vector<DeathRecord> m_deaths;
};

} // namespace hub::meter
