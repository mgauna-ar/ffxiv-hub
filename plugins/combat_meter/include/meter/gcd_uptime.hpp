#pragma once

#include "meter/types.hpp"
#include <cstdint>
#include <span>

namespace hub::meter {

/// One cast on the global cooldown, stamped when it reached the client, with its
/// action's GCD and cast time from the Action sheet (game::gcd_timing).
struct GcdCast {
    uint64_t at_us{0};
    uint16_t recast_100ms{0};
    uint16_t cast_100ms{0};
};

/// How a player kept the global cooldown rolling over a pull.
struct GcdUptime {
    /// The GCD their casts suggest, for an action with a 2.5 s recast.
    double estimate_s{0.0};
    /// Share of the pull the GCD was rolling, 0-100.
    double uptime_pct{0.0};
};

/**
 * @brief Estimates a player's GCD and how much of [start_us, end_us] it covered.
 *
 * A spell with a cast time reaches the client when its cast ends, so its press is
 * worked out backwards from the Action sheet's cast time. One that could not have been
 * hardcast since the previous GCD was cast instantly (Swiftcast, Dualcast, a proc).
 * `casts` is in the order they arrived.
 */
[[nodiscard]] GcdUptime gcd_uptime(std::span<const GcdCast> casts, uint64_t start_us, uint64_t end_us);

} // namespace hub::meter
