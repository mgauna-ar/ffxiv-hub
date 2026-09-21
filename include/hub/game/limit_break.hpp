#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace hub::game {

/// Synthetic combatant id holding Limit Break damage. The game reports the casting
/// player as the source, so the meter needs an id of its own to move that damage off
/// the player's row; picked above every real entity id so it cannot collide with one.
/// Not 0xE0000000 - that is the game's "no owner / not networked" sentinel.
constexpr uint32_t LIMIT_BREAK_COMBATANT_ID = 0xFFFFFFFA;

/// Every action the game files under ActionCategory 9 ("Limit Break"), sorted by id.
/// Regenerate with:
///   curl "https://v2.xivapi.com/api/search?sheets=Action&query=ActionCategory%3D9&limit=200&fields=Name"
/// The list includes duty-action and NPC limit breaks, so callers must also check that
/// the source is friendly before treating a hit as party limit break damage.
constexpr std::array<uint32_t, 74> LIMIT_BREAK_ACTIONS{{
    197,    // Shield Wall
    198,    // Stronghold
    199,    // Last Bastion
    200,    // Braver
    201,    // Bladedance
    202,    // Final Heaven
    203,    // Skyshard
    204,    // Starstorm
    205,    // Meteor
    206,    // Healing Wind
    207,    // Breath of the Earth
    208,    // Pulse of Life
    4238,   // Big Shot
    4239,   // Desperado
    4240,   // Land Waker
    4241,   // Dark Force
    4242,   // Dragonsong Dive
    4243,   // Chimatsuri
    4244,   // Sagittarius Arrow
    4245,   // Satellite Beam
    4246,   // Teraflare
    4247,   // Angel Feathers
    4248,   // Astral Stasis
    4313,   // (unnamed)
    7801,   // (unnamed)
    7861,   // Doom of the Living
    7862,   // Vermilion Scourge
    10001,  // Ungarmax
    10002,  // Ungarmax
    10894,  // Starstorm
    11193,  // Starstorm
    16578,  // Falling Star
    17084,  // Hypershot
    17105,  // Gunmetal Soul
    17106,  // Crimson Lotus
    17211,  // Arrow of Fortitude
    17221,  // the Reach of Darkness
    17259,  // Eternal Wind
    17576,  // Final Bastion
    17627,  // Out of the Labyrinth
    17993,  // Big Shot
    17994,  // Desperado
    17995,  // Skyshard
    17996,  // Starstorm
    17997,  // Braver
    17998,  // Bladedance
    18781,  // Dragonshadow Dive
    18782,  // Dragonshadow Dive
    24858,  // the End
    24859,  // Techne Makre
    25526,  // Dragonshadow Dive
    25527,  // Vermilion Scourge
    25528,  // Meteor
    25529,  // Astral Stasis
    25640,  // Teraflare
    26496,  // Company Aegis
    26702,  // Penitent Blade
    28277,  // Thelema
    28329,  // Vermilion Pledge
    29775,  // Final Heaven
    29936,  // Diamond Dust
    29986,  // Ashen Thread
    31372,  // Gutdriver
    32618,  // Aetheric Ray
    34386,  // Immortal Flame
    34866,  // World-swallower
    34867,  // Chromatic Fantasy
    37129,  // Dawnlit Conviction
    37352,  // Dawnlit Conviction
    37509,  // Aetheric Sever
    38956,  // Aetheric Sever
    38994,  // Chromatic Fantasy
    44287,  // Cleansing Storm
    44288,  // Mercy's Justice
}};

/// True for an action in the game's Limit Break category.
[[nodiscard]] constexpr bool is_limit_break_action(uint32_t action_id) noexcept {
    return std::binary_search(LIMIT_BREAK_ACTIONS.begin(), LIMIT_BREAK_ACTIONS.end(), action_id);
}

} // namespace hub::game
