#pragma once

#include <cstdint>

namespace hub::game {

/// FFXIV entity ID convention: monsters and NPCs have bit 0x40000000 set (0x40xxxxxx).
constexpr uint32_t ENTITY_ID_MONSTER_BIT = 0x40000000;

/// The game's placeholder id: written for an actor with no owner, and for the target
/// slot of an effect that hit nothing. Never a real actor.
constexpr uint32_t NO_ENTITY_ID = 0xE0000000;

/// True when an id names an actor that can actually hold stats.
[[nodiscard]] constexpr bool is_real_entity_id(uint32_t entity_id) noexcept {
    return entity_id != 0 && entity_id != NO_ENTITY_ID;
}

[[nodiscard]] constexpr bool is_monster_entity_id(uint32_t entity_id) noexcept {
    return (entity_id & ENTITY_ID_MONSTER_BIT) != 0;
}

} // namespace hub::game
