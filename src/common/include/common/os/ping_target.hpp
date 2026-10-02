#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace hub::os {

/// One row of the TCP connection table, as NetworkMonitor reads it.
struct TcpConnection {
    uint32_t pid{0};
    uint32_t remote_addr{0}; ///< first octet in the low byte, as Windows hands it over
    uint16_t remote_port{0}; ///< host byte order
    bool established{false};
};

/// Where a ping goes.
enum class PingSource : uint8_t {
    None,
    /// The server the game holds a connection to.
    GameServer,
    /// The lobby server of the data center the character is on. A gaming VPN
    /// hides or rewrites the game's connections, so this is all that is left.
    DataCenterLobby,
};

struct PingTarget {
    PingSource source{PingSource::None};
    uint32_t address{0};
};

/// What to ping, best first: at most the game server, then the data center lobby.
struct PingCandidates {
    std::array<PingTarget, 2> targets{};
    size_t count{0};

    [[nodiscard]] std::span<const PingTarget> view() const noexcept { return {targets.data(), count}; }
};

/// True for a port Square Enix lists for the game's lobby, chat and zone servers.
[[nodiscard]] bool is_game_server_port(uint16_t port) noexcept;

/// The first established connection of `pid` to a game server port, never to a
/// loopback or unset address. 0 when there is none.
[[nodiscard]] uint32_t find_game_server(std::span<const TcpConnection> connections, uint32_t pid) noexcept;

/// The game server, then the lobby of `world`'s data center (0 for unknown), each
/// only when known, and the lobby only when it is a different address.
[[nodiscard]] PingCandidates ping_candidates(std::span<const TcpConnection> connections, uint32_t pid,
                                             uint16_t world) noexcept;

/// "a.b.c.d" for an address in the layout above.
[[nodiscard]] std::string format_ipv4(uint32_t address);

} // namespace hub::os
