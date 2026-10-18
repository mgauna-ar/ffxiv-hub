#pragma once

#include "meter/types.hpp"
#include <cstdint>
#include <unordered_map>

namespace hub::meter {

/// How much of a hit on an enemy went past the HP it had left, which damage done
/// leaves out.
///
/// The client applies a hit's HP only when its effect result arrives, a moment
/// after the effect, so the HP read as an effect lands still holds every earlier hit
/// that has not applied yet. The ledger keeps that damage per enemy and takes it off
/// the HP read. Each drop in the HP read since the last hit is taken as some of it
/// applying, and anything older than kPendingTtlUs as all of it. A drop from a DoT
/// tick, which the ledger never books, counts as one too, so it errs towards less
/// overkill, never more.
///
/// Used only on the game's main thread, where every effect arrives.
class OverkillLedger {
public:
    /// Longer than most effects wait for their result. A Limit Break 3 can wait 2.2 s,
    /// and its damage is forgotten a little early.
    static constexpr uint64_t kPendingTtlUs = 2'000'000;

    /// The part of `damage` past what `target` had left: `hp`, read from the client
    /// as the hit's effect arrived, less the damage still on its way. Books the rest
    /// as on its way.
    [[nodiscard]] uint32_t book(EntityId target, uint32_t damage, uint32_t hp, uint64_t now_us);

    [[nodiscard]] size_t tracked_count() const noexcept { return m_targets.size(); }

private:
    struct Pending {
        uint32_t hp_seen{0};
        uint64_t amount{0};
        uint64_t booked_us{0};
    };
    /// Enemies hit within kPendingTtlUs; older ones are pruned once there are more.
    static constexpr size_t kPruneAbove = 64;

    std::unordered_map<EntityId, Pending> m_targets;
};

} // namespace hub::meter
