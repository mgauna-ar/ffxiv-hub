#pragma once

#include "hub/game/world.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

namespace hub::game {

/// An IPv4 address the way Windows hands one over (IPAddr, in_addr::S_addr): the
/// first octet in the low byte.
[[nodiscard]] constexpr uint32_t ipv4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) noexcept {
    return static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) |
           (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24);
}

struct LobbyServer {
    uint8_t data_center; ///< WorldDCGroupType row
    uint32_t address;    ///< see ipv4()
};

/// Each data center's lobby server, which is hosted alongside its worlds. Hand-curated
/// from the published lobby addresses: the sheets do not carry them, and they change
/// only when a data center is added or moved, not with a patch. Sorted by data center.
inline constexpr std::array<LobbyServer, 12> LOBBY_SERVERS{{
    { 1, ipv4(119, 252, 36, 6)},   // Elemental
    { 2, ipv4(119, 252, 36, 7)},   // Gaia
    { 3, ipv4(119, 252, 36, 8)},   // Mana
    { 4, ipv4(204, 2, 29, 6)},     // Aether
    { 5, ipv4(204, 2, 29, 7)},     // Primal
    { 6, ipv4(80, 239, 145, 6)},   // Chaos
    { 7, ipv4(80, 239, 145, 7)},   // Light
    { 8, ipv4(204, 2, 29, 8)},     // Crystal
    { 9, ipv4(153, 254, 80, 103)}, // Materia
    {10, ipv4(119, 252, 36, 9)},   // Meteor
    {11, ipv4(204, 2, 29, 9)},     // Dynamis
    {12, ipv4(80, 239, 145, 8)},   // Shadow
}};

/// The lobby server of the data center `world` is on; nullopt for a world the tables
/// lack or a data center without a known lobby.
[[nodiscard]] constexpr std::optional<uint32_t> lobby_address(uint16_t world) noexcept {
    const uint8_t data_center = world_data_center(world);
    const auto it = std::lower_bound(LOBBY_SERVERS.begin(), LOBBY_SERVERS.end(), data_center,
        [](const LobbyServer& entry, uint8_t value) { return entry.data_center < value; });
    if (data_center == 0 || it == LOBBY_SERVERS.end() || it->data_center != data_center) {
        return std::nullopt;
    }
    return it->address;
}

} // namespace hub::game
