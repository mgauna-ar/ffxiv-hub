#include "common/os/network_monitor.hpp"
#include "common/os/logger.hpp"
#include "common/os/unique_handle.hpp"
#include "hub/game/world.hpp"
#include <algorithm>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <icmpapi.h>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")
#endif

namespace hub::os {

namespace {
// How often the worker wakes to check m_running while it waits out the interval.
#ifdef _WIN32
constexpr uint32_t kSleepStepMs = 100;
#else
constexpr uint32_t kSleepStepMs = 50;
#endif

// Probes in a row without a reply before that goes in the log: one lost echo is noise.
constexpr uint32_t kSilentProbesBeforeLog = 3;

std::string describe(const PingTarget& target, uint16_t world) {
    const std::string address = format_ipv4(target.address);
    if (target.source == PingSource::DataCenterLobby) {
        const std::string_view name = game::data_center_name(game::world_data_center(world));
        return std::string(name.empty() ? "data center" : name) + " lobby " + address;
    }
    return "game server " + address;
}

#ifdef _WIN32
/// An IcmpCreateFile handle, which only IcmpCloseHandle may close.
struct IcmpHandleTraits {
    using pointer = HANDLE;
    static pointer empty() noexcept { return INVALID_HANDLE_VALUE; }
    static bool is_valid(pointer handle) noexcept { return handle != INVALID_HANDLE_VALUE && handle != nullptr; }
    static void close(pointer handle) noexcept { IcmpCloseHandle(handle); }
};
using UniqueIcmpHandle = BasicUniqueHandle<IcmpHandleTraits>;
#endif
} // namespace

NetworkMonitor::NetworkMonitor(uint32_t target_pid)
    : m_target_pid(target_pid) {}

NetworkMonitor::~NetworkMonitor() {
    stop();
}

void NetworkMonitor::start(uint32_t target_pid) {
    if (target_pid != 0) {
        m_target_pid.store(target_pid);
    }
    if (m_running.exchange(true)) {
        return; // Already running
    }

    m_worker_thread = std::make_unique<std::thread>(&NetworkMonitor::worker_loop, this);
}

void NetworkMonitor::stop() {
    if (!m_running.exchange(false)) {
        return;
    }

    if (m_worker_thread && m_worker_thread->joinable()) {
        m_worker_thread->join();
    }
    m_worker_thread.reset();
    m_current_ping_ms.store(-1.0);
}

void NetworkMonitor::set_target_pid(uint32_t target_pid) {
    if (m_target_pid.exchange(target_pid) != target_pid) {
        m_fallback_world.store(0);
    }
    if (target_pid == 0) {
        m_current_ping_ms.store(-1.0);
    }
}

uint32_t NetworkMonitor::target_pid() const noexcept {
    return m_target_pid.load();
}

double NetworkMonitor::get_current_ping_ms() const noexcept {
    return m_current_ping_ms.load();
}

bool NetworkMonitor::is_running() const noexcept {
    return m_running.load();
}

void NetworkMonitor::set_mock_ping(double ping_ms) noexcept {
    m_current_ping_ms.store(ping_ms);
}

void NetworkMonitor::note_outcome(std::span<const PingTarget> candidates, const PingTarget* answered,
                                  uint16_t world) {
    std::string outcome;
    if (answered != nullptr) {
        m_silent_probes = 0;
        outcome = "pinging " + describe(*answered, world);
        if (answered->source == PingSource::DataCenterLobby && candidates.size() > 1) {
            outcome += " (the game server does not answer)";
        } else if (answered->source == PingSource::DataCenterLobby) {
            outcome += " (no game server connection is visible)";
        }
    } else if (candidates.empty()) {
        outcome = "nothing to ping: no game server connection is visible and the character's world is unknown";
    } else {
        if (++m_silent_probes < kSilentProbesBeforeLog) return;
        outcome = "no reply from";
        for (size_t i = 0; i < candidates.size(); ++i) {
            outcome += (i == 0 ? " " : " or ") + describe(candidates[i], world);
        }
    }
    if (outcome != m_logged_outcome) {
        Logger::info("Network ping: " + outcome);
        m_logged_outcome = std::move(outcome);
    }
}

void NetworkMonitor::worker_loop() {
    while (m_running.load()) {
        if (const uint32_t pid = m_target_pid.load(); pid != 0) {
            probe_target(pid);
        } else {
            m_current_ping_ms.store(-1.0);
        }

        const uint32_t interval = m_probe_interval_ms.load();
        const uint32_t step = kSleepStepMs;
        for (uint32_t waited = 0; waited < interval && m_running.load(); waited += step) {
            std::this_thread::sleep_for(std::chrono::milliseconds(step));
        }
    }
}

#ifdef _WIN32

bool NetworkMonitor::probe_target(uint32_t pid) {
    const uint16_t world = m_fallback_world.load();

    // The game's TCP connections. The table can grow between the size query and the
    // read, hence the retries. A failed read leaves only the lobby to ping.
    std::vector<TcpConnection> connections;
    std::vector<uint8_t> buffer;
    DWORD size = 0;
    DWORD status = ERROR_INSUFFICIENT_BUFFER;
    for (int attempt = 0; attempt < 3 && status == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
        buffer.resize(size);
        status = GetExtendedTcpTable(buffer.empty() ? nullptr : buffer.data(), &size, FALSE, AF_INET,
                                     TCP_TABLE_OWNER_PID_ALL, 0);
    }
    if (status == NO_ERROR && !buffer.empty()) {
        const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
        connections.reserve(table->dwNumEntries);
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
            const auto& row = table->table[i];
            connections.push_back(TcpConnection{
                row.dwOwningPid, row.dwRemoteAddr,
                ntohs(static_cast<u_short>(row.dwRemotePort)),
                row.dwState == MIB_TCP_STATE_ESTAB});
        }
    }

    const PingCandidates candidates = ping_candidates(connections, pid, world);
    const PingTarget* answered = nullptr;
    double ping_result = -1.0;

    if (candidates.count != 0) {
        const UniqueIcmpHandle icmp(IcmpCreateFile());
        if (icmp) {
            char send_data[32] = "FFXIV_HUB_PING";
            const DWORD reply_size = sizeof(ICMP_ECHO_REPLY) + sizeof(send_data) + 8;
            std::vector<uint8_t> reply_buf(reply_size);

            for (const PingTarget& target : candidates.view()) {
                const DWORD replies = IcmpSendEcho(
                    icmp.get(), target.address, send_data, static_cast<WORD>(sizeof(send_data)),
                    nullptr, reply_buf.data(), reply_size,
                    1000 // 1-second timeout
                );
                const auto* echo_reply = reinterpret_cast<const ICMP_ECHO_REPLY*>(reply_buf.data());
                if (replies > 0 && echo_reply->Status == IP_SUCCESS) {
                    ping_result = static_cast<double>(echo_reply->RoundTripTime);
                    answered = &target;
                    break;
                }
            }
        }
    }

    m_current_ping_ms.store(ping_result);
    note_outcome(candidates.view(), answered, world);
    return ping_result >= 0.0;
}

#else // !_WIN32

bool NetworkMonitor::probe_target(uint32_t) {
    // If a mock ping was manually set, maintain it, otherwise default to 32.0ms mock
    double current = m_current_ping_ms.load();
    if (current < 0.0) {
        current = 32.0;
        m_current_ping_ms.store(current);
    }

    return true;
}

#endif

} // namespace hub::os
