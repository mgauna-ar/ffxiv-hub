#include "common/os/ping_target.hpp"
#include "hub/game/data_centers.hpp"

namespace hub::os {

namespace {

struct PortRange {
    uint16_t first;
    uint16_t last;
};

// The TCP ports Square Enix publishes for the game. 80 and 443 are the launcher's.
constexpr std::array<PortRange, 4> kGamePorts{{
    {54992, 54994},
    {55006, 55007},
    {55021, 55040},
    {55296, 55551},
}};

bool is_loopback_or_unset(uint32_t address) noexcept {
    return address == 0 || (address & 0xFF) == 127;
}

} // namespace

bool is_game_server_port(uint16_t port) noexcept {
    for (const PortRange& range : kGamePorts) {
        if (port >= range.first && port <= range.last) return true;
    }
    return false;
}

uint32_t find_game_server(std::span<const TcpConnection> connections, uint32_t pid) noexcept {
    for (const TcpConnection& row : connections) {
        if (row.pid == pid && row.established && !is_loopback_or_unset(row.remote_addr) &&
            is_game_server_port(row.remote_port)) {
            return row.remote_addr;
        }
    }
    return 0;
}

PingCandidates ping_candidates(std::span<const TcpConnection> connections, uint32_t pid,
                               uint16_t world) noexcept {
    PingCandidates out;
    if (const uint32_t server = find_game_server(connections, pid); server != 0) {
        out.targets[out.count++] = {PingSource::GameServer, server};
    }
    if (const auto lobby = game::lobby_address(world);
        lobby && (out.count == 0 || out.targets[0].address != *lobby)) {
        out.targets[out.count++] = {PingSource::DataCenterLobby, *lobby};
    }
    return out;
}

std::string format_ipv4(uint32_t address) {
    std::string text;
    for (int shift = 0; shift < 32; shift += 8) {
        if (shift != 0) text += '.';
        text += std::to_string((address >> shift) & 0xFF);
    }
    return text;
}

} // namespace hub::os
