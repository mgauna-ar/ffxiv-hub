#pragma once

#include "meter/types.hpp"
#include "common/ipc/protocol.hpp"
#include "meter/combatant_registry.hpp"
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <string>
#include <string_view>

namespace hub::meter {

class MetricsAccumulator {
public:
    MetricsAccumulator() = default;

    /// Records a combat action (damage or heal) and attributes it to the proper owner.
    void record_action(const ipc::CombatActionPacket& packet, const CombatantRegistry& registry);

    /// Records a periodic status tick (DoT damage or HoT heal).
    void record_status_tick(const ipc::StatusTickPacket& packet, const CombatantRegistry& registry);

    /// Recalculates DPS, HPS, and damage share percentages based on elapsed active seconds.
    /// If registry is provided, automatically consolidates pet stats into owners.
    void recalculate(double duration_seconds, const CombatantRegistry* registry = nullptr);

    /// Merges all metrics from one combatant into another (e.g. late pet attribution).
    /// A missing destination row is created from the registry's entry for it.
    void merge_combatants(EntityId from_id, EntityId to_id, const CombatantRegistry& registry);

    /// Lookup statistics for a specific combatant.
    [[nodiscard]] const CombatantStats* find_stats(EntityId entity_id) const;

    /// Returns list of all tracked combatants sorted by DPS descending.
    [[nodiscard]] std::vector<CombatantStats> sorted_by_dps(bool friendly_only = true) const;

    /// Returns list of all tracked combatants sorted by HPS descending.
    [[nodiscard]] std::vector<CombatantStats> sorted_by_hps(bool friendly_only = true) const;

    /// Read-only map of all combatant stats.
    [[nodiscard]] const std::unordered_map<EntityId, CombatantStats>& combatants() const noexcept {
        return m_combatants;
    }

    /// Encounter-wide aggregated metrics.
    [[nodiscard]] uint64_t total_damage() const noexcept { return m_total_damage; }
    [[nodiscard]] uint64_t total_healing() const noexcept { return m_total_healing; }
    [[nodiscard]] uint64_t total_effective_healing() const noexcept { return m_total_effective_healing; }
    [[nodiscard]] uint64_t total_overhealing() const noexcept { return m_total_overhealing; }
    [[nodiscard]] double total_dps() const noexcept { return m_total_dps; }
    [[nodiscard]] double total_hps() const noexcept { return m_total_hps; }
    [[nodiscard]] double overheal_pct() const noexcept;

    /// Resets all metrics for a new encounter.
    void clear();

private:
    CombatantStats& get_or_create_stats(EntityId entity_id, const CombatantRegistry& registry);

    /// Shared body for the effect types that carry a damage value: full hits, and the
    /// partially mitigated Blocked/Parried variants.
    void record_damage_hit(
        CombatantStats& stats,
        const ipc::CombatActionPacket& packet,
        HitSeverity severity,
        bool is_pet_hit,
        bool source_friendly,
        const CombatantRegistry& registry
    );

    std::unordered_map<EntityId, CombatantStats> m_combatants;
    uint64_t m_total_damage{0};
    uint64_t m_total_healing{0};
    uint64_t m_total_effective_healing{0};
    uint64_t m_total_overhealing{0};
    double m_total_dps{0.0};
    double m_total_hps{0.0};
};

} // namespace hub::meter
