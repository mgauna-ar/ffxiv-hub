#pragma once

#include "meter/types.hpp"
#include "common/ipc/protocol.hpp"
#include <cstdint>
#include <span>
#include <unordered_map>

namespace hub::meter {

/// Base crit and direct hit rates per player, estimated from the hits no rate buff or
/// guarantee touched. Weighted toward a typical rate until enough hits are seen.
class RateEstimator {
public:
    static constexpr double kPriorCrit = 0.22;
    static constexpr double kPriorDirectHit = 0.25;
    static constexpr double kPriorWeight = 100.0;
    static constexpr double kMinRate = 0.05;
    static constexpr double kMaxRate = 0.60;

    void observe(EntityId entity, bool crit, bool direct_hit);
    [[nodiscard]] double crit_rate(EntityId entity) const;
    [[nodiscard]] double direct_hit_rate(EntityId entity) const;

private:
    struct Counts {
        uint32_t hits{0};
        uint32_t crits{0};
        uint32_t direct_hits{0};
    };
    std::unordered_map<EntityId, Counts> m_counts;
};

/// Strength of the buffs whose strength the applying action decides: steps danced
/// for Standard and Technical Finish, codas held for Radiant Finale.
class BuffStrengths {
public:
    /// Every action a player uses, in order: songs grant codas, finishes set strengths.
    void on_action(EntityId source, ActionId action_id);

    /// Strength of `status_id` as `giver` last applied it, or `fallback` when unseen.
    [[nodiscard]] uint16_t permille(EntityId giver, uint16_t status_id, uint16_t fallback) const;

private:
    [[nodiscard]] static uint64_t key(EntityId giver, uint16_t status_id) noexcept {
        return (static_cast<uint64_t>(giver) << 16) | status_id;
    }
    std::unordered_map<uint64_t, uint16_t> m_applied;
    /// Songs sung since the bard's last Radiant Finale, one bit per song.
    std::unordered_map<EntityId, uint8_t> m_codas;
};

/// One hit as the attribution sees it. A pet's hit is its owner's.
struct AttributedHit {
    EntityId source{0};
    Role source_role{Role::None};
    ActionId action_id{0};
    uint32_t damage{0};
    bool crit{false};
    bool direct_hit{false};
    /// A DoT tick: the game reports no crit or direct hit, so rate buffs are
    /// credited by their expected value.
    bool is_tick{false};
};

struct Attribution {
    ipc::CombatBuffCredits credits{};
    /// No rate status on either side and nothing guaranteed: the hit shows the
    /// attacker's own crit and direct hit rates.
    bool clean{false};
};

/**
 * @brief Splits the damage external buffs added to a hit among the players who gave them.
 *
 * `on_source` holds the attacker's statuses (its owner's for a pet) and `on_target` the
 * target's; anything that is not a raid buff is ignored. Each buff's multiplier is its
 * damage bonus, or the share of a crit or direct hit its rate bonus accounts for, and
 * the damage above what the hit would have done without them is split in proportion to
 * the logarithm of each multiplier. Buffs the attacker gave itself earn nothing.
 */
[[nodiscard]] Attribution attribute_hit(
    const AttributedHit& hit,
    std::span<const ipc::CombatStatusEntry> on_source,
    std::span<const ipc::CombatStatusEntry> on_target,
    const RateEstimator& rates,
    const BuffStrengths& strengths
);

/// Total a hit's credits hand to other players.
[[nodiscard]] uint64_t credited_total(const ipc::CombatBuffCredits& credits) noexcept;

/// True for a status attribute_hit reads, a raid buff or one that guarantees a hit, or
/// one a combined tick is split across (hub::game::TICK_STATUSES). The payload keeps
/// only these when it snapshots a status list for a hit or a tick.
[[nodiscard]] bool is_attribution_status(uint32_t status_id) noexcept;

} // namespace hub::meter
