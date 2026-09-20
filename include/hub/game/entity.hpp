#pragma once

#include <cstdint>

namespace hub::game {

/// FFXIV entity ID convention: monsters and NPCs have bit 0x40000000 set (0x40xxxxxx).
constexpr uint32_t ENTITY_ID_MONSTER_BIT = 0x40000000;

[[nodiscard]] constexpr bool is_monster_entity_id(uint32_t entity_id) noexcept {
    return (entity_id & ENTITY_ID_MONSTER_BIT) != 0;
}

} // namespace hub::game
