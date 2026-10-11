#pragma once

#include "common/ipc/protocol.hpp"
#include "hub/game/tick_statuses.hpp"
#include "meter/types.hpp"
#include <cstdint>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hub::meter {

/// What one status from one source put into a combined tick.
struct TickShare {
    EntityId source{0};
    uint16_t status_id{0};
    uint32_t amount{0};
    /// The part of `amount` that overhealed; 0 for damage.
    uint32_t overheal{0};
};

/// `total` split in proportion to `weights`, by largest remainder, so the parts add up
/// to `total` exactly. A tie goes to the earlier weight. All zero weights split evenly.
[[nodiscard]] std::vector<uint32_t> split_exact(uint32_t total, std::span<const double> weights);

/// Splits the server's combined ticks between the statuses on the target.
///
/// The server sums every damage (or healing) over time status on a target into one tick
/// that names no status and one source, so the tick alone cannot say whose it was. Each
/// status on the target when it arrives gets a share by its potency times its source's
/// strength: damage or healing per potency point, learned from ticks where every such
/// status on the target came from one source, which makes that tick exactly theirs.
/// A source not learned yet takes the mean of those that are. Kept for the game
/// session, like RateEstimator.
class TickSplitter {
public:
    /// Weight of a new observation in a source's strength.
    static constexpr double kLearnRate = 0.1;

    /// Shares of a combined tick of `amount`, `overheal` of it overhealed, across the
    /// `kind` statuses in `on_target`. Empty when none of them is a known tick status
    /// with a real source: the caller keeps the tick whole. The shares add up to
    /// `amount` and `overheal` exactly; a share of nothing is left out.
    [[nodiscard]] std::vector<TickShare> split(game::TickKind kind, uint32_t amount, uint32_t overheal,
                                               std::span<const ipc::CombatStatusEntry> on_target);

    /// A status that ticked on a packet of its own, as ground effects do, is never part
    /// of a combined tick, so it takes no share from one again.
    void note_own_tick(uint16_t status_id);

    /// Damage or healing per potency point `source`'s ticks deal; the mean of every
    /// learned source when it has none, and 1 when no source has.
    [[nodiscard]] double strength(EntityId source, game::TickKind kind) const;

private:
    [[nodiscard]] static uint64_t key(EntityId source, game::TickKind kind) noexcept {
        return (static_cast<uint64_t>(source) << 1) | static_cast<uint64_t>(kind == game::TickKind::Heal);
    }

    std::unordered_map<uint64_t, double> m_strength;
    std::unordered_set<uint16_t> m_own_tick;
};

} // namespace hub::meter
