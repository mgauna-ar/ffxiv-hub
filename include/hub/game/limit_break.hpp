#pragma once

#include <cstdint>

namespace hub::game {

/// The game's pseudo-entity id. Written instead of 0 for an actor with no owner,
/// and reported as the source of Limit Break casts.
constexpr uint32_t LIMIT_BREAK_ENTITY_ID = 0xE0000000;

/// True for a Limit Break action.
[[nodiscard]] constexpr bool is_limit_break_action(uint32_t action_id) noexcept {
    // Shared tier actions: Shield Wall / Braver / Skyshard / Healing Wind and upgrades.
    if (action_id >= 197 && action_id <= 208) {
        return true;
    }
    // Job-specific LB3s (Dragonsong Dive, Chimatsuri, The End, ...) use ids outside
    // that block. Add them here once a live pull confirms the id: a wrong one would
    // divert a real action's damage to the Limit Break row, so do not guess.
    return false;
}

} // namespace hub::game
