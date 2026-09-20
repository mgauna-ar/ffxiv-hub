#include "common/os/network_monitor.hpp"
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

namespace hub::os {

NetworkMonitor::NetworkMonitor(uint32_t target_pid)
    : m_target_pid(target_pid) {}

NetworkMonitor::~NetworkMonitor() {
    stop();
}

NetworkMonitor::NetworkMonitor(NetworkMonitor&& other) noexcept {
    stop();
    m_target_pid.store(other.m_target_pid.load());
    m_probe_interval_ms.store(other.m_probe_interval_ms.load());
    m_current_ping_ms.store(other.m_current_ping_ms.load());
    m_has_connection.store(other.m_has_connection.load());

    {
        std::lock_guard<std::mutex> lock(other.m_ip_mutex);
        m_server_ip = std::move(other.m_server_ip);
    }
    {
        std::lock_guard<std::mutex> lock(other.m_callback_mutex);
        m_on_ping_update = std::move(other.m_on_ping_update);
    }

    if (other.m_running.load()) {
        other.stop();
        start(m_target_pid.load());
    }
}

NetworkMonitor& NetworkMonitor::operator=(NetworkMonitor&& other) noexcept {
    if (this != &other) {
        stop();
        m_target_pid.store(other.m_target_pid.load());
        m_probe_interval_ms.store(other.m_probe_interval_ms.load());
        m_current_ping_ms.store(other.m_current_ping_ms.load());
        m_has_connection.store(other.m_has_connection.load());

        {
            std::lock_guard<std::mutex> lock(other.m_ip_mutex);
            m_server_ip = std::move(other.m_server_ip);
        }
        {
            std::lock_guard<std::mutex> lock(other.m_callback_mutex);
            m_on_ping_update = std::move(other.m_on_ping_update);
        }

        if (other.m_running.load()) {
            other.stop();
            start(m_target_pid.load());
        }
    }
    return *this;
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
    m_has_connection.store(false);
    m_current_ping_ms.store(-1.0);
}

void NetworkMonitor::set_target_pid(uint32_t target_pid) {
    m_target_pid.store(target_pid);
    if (target_pid == 0) {
        m_has_connection.store(false);
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

bool NetworkMonitor::has_valid_connection() const noexcept {
    return m_has_connection.load();
}

std::string NetworkMonitor::get_server_ip() const {
    std::lock_guard<std::mutex> lock(m_ip_mutex);
    return m_server_ip;
}

void NetworkMonitor::set_on_ping_update(std::function<void(double)> cb) {
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    m_on_ping_update = std::move(cb);
}

void NetworkMonitor::set_mock_ping(double ping_ms) noexcept {
    m_current_ping_ms.store(ping_ms);
    m_has_connection.store(ping_ms >= 0.0);
    std::function<void(double)> cb;
    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        cb = m_on_ping_update;
    }
    if (cb && ping_ms >= 0.0) {
        cb(ping_ms);
    }
}

void NetworkMonitor::worker_loop() {
    while (m_running.load()) {
        probe_once();

        const uint32_t interval = m_probe_interval_ms.load();
        const uint32_t step = 100;
        for (uint32_t waited = 0; waited < interval && m_running.load(); waited += step) {
            std::this_thread::sleep_for(std::chrono::milliseconds(step));
        }
    }
}

bool NetworkMonitor::probe_once() {
    const uint32_t pid = m_target_pid.load();
    if (pid == 0) {
        m_has_connection.store(false);
        m_current_ping_ms.store(-1.0);
        return false;
    }

    // 1. Locate active game TCP connection via GetExtendedTcpTable
    DWORD dwSize = 0;
    GetExtendedTcpTable(nullptr, &dwSize, TRUE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    if (dwSize == 0) {
        return false;
    }

    std::vector<uint8_t> buffer(dwSize);
    auto* pTcpTable = reinterpret_cast<PMIB_TCPTABLE_OWNER_PID>(buffer.data());

    if (GetExtendedTcpTable(pTcpTable, &dwSize, TRUE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) != NO_ERROR) {
        return false;
    }

    DWORD target_ip = 0;

    for (DWORD i = 0; i < pTcpTable->dwNumEntries; ++i) {
        const auto& row = pTcpTable->table[i];
        if (row.dwOwningPid != pid || row.dwState != MIB_TCP_STATE_ESTAB) {
            continue;
        }

        // Filter out loopback (127.x.x.x) and invalid 0.0.0.0 addresses
        const uint32_t remote_addr = row.dwRemoteAddr;
        if ((remote_addr & 0xFF) == 127 || remote_addr == 0) {
            continue;
        }

        const uint16_t remote_port = ntohs(static_cast<u_short>(row.dwRemotePort));

        // FFXIV lobby and game zone servers listen between 54992 and 55007
        if (remote_port >= 54992 && remote_port <= 55007) {
            target_ip = remote_addr;
            break;
        }

        // Generic fallback to any established remote connection
        if (target_ip == 0) {
            target_ip = remote_addr;
        }
    }

    if (target_ip == 0) {
        m_has_connection.store(false);
        return false;
    }

    // Convert IP to string for diagnostics and tracking
    in_addr addr;
    addr.s_addr = target_ip;
    char ip_str[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &addr, ip_str, INET_ADDRSTRLEN);

    {
        std::lock_guard<std::mutex> lock(m_ip_mutex);
        m_server_ip = ip_str;
    }
    m_has_connection.store(true);

    // 2. Perform ICMP Echo Probe
    HANDLE hIcmpFile = IcmpCreateFile();
    if (hIcmpFile == INVALID_HANDLE_VALUE) {
        m_current_ping_ms.store(-1.0);
        return false;
    }

    char send_data[32] = "FFXIV_HUB_PING";
    const DWORD reply_size = sizeof(ICMP_ECHO_REPLY) + sizeof(send_data) + 8;
    std::vector<uint8_t> reply_buf(reply_size);

    DWORD replies = IcmpSendEcho(
        hIcmpFile,
        target_ip,
        send_data,
        static_cast<WORD>(sizeof(send_data)),
        nullptr,
        reply_buf.data(),
        reply_size,
        1000 // 1-second timeout
    );

    double ping_result = -1.0;
    if (replies > 0) {
        auto* echo_reply = reinterpret_cast<PICMP_ECHO_REPLY>(reply_buf.data());
        if (echo_reply->Status == IP_SUCCESS) {
            ping_result = static_cast<double>(echo_reply->RoundTripTime);
        }
    }

    IcmpCloseHandle(hIcmpFile);

    m_current_ping_ms.store(ping_result);

    std::function<void(double)> cb;
    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        cb = m_on_ping_update;
    }
    if (cb) {
        cb(ping_result);
    }

    return (ping_result >= 0.0);
}

} // namespace hub::os

#else // !_WIN32

namespace hub::os {

NetworkMonitor::NetworkMonitor(uint32_t target_pid)
    : m_target_pid(target_pid) {}

NetworkMonitor::~NetworkMonitor() {
    stop();
}

NetworkMonitor::NetworkMonitor(NetworkMonitor&& other) noexcept {
    stop();
    m_target_pid.store(other.m_target_pid.load());
    m_probe_interval_ms.store(other.m_probe_interval_ms.load());
    m_current_ping_ms.store(other.m_current_ping_ms.load());
    m_has_connection.store(other.m_has_connection.load());

    {
        std::lock_guard<std::mutex> lock(other.m_ip_mutex);
        m_server_ip = std::move(other.m_server_ip);
    }
    {
        std::lock_guard<std::mutex> lock(other.m_callback_mutex);
        m_on_ping_update = std::move(other.m_on_ping_update);
    }

    if (other.m_running.load()) {
        other.stop();
        start(m_target_pid.load());
    }
}

NetworkMonitor& NetworkMonitor::operator=(NetworkMonitor&& other) noexcept {
    if (this != &other) {
        stop();
        m_target_pid.store(other.m_target_pid.load());
        m_probe_interval_ms.store(other.m_probe_interval_ms.load());
        m_current_ping_ms.store(other.m_current_ping_ms.load());
        m_has_connection.store(other.m_has_connection.load());

        {
            std::lock_guard<std::mutex> lock(other.m_ip_mutex);
            m_server_ip = std::move(other.m_server_ip);
        }
        {
            std::lock_guard<std::mutex> lock(other.m_callback_mutex);
            m_on_ping_update = std::move(other.m_on_ping_update);
        }

        if (other.m_running.load()) {
            other.stop();
            start(m_target_pid.load());
        }
    }
    return *this;
}

void NetworkMonitor::start(uint32_t target_pid) {
    if (target_pid != 0) {
        m_target_pid.store(target_pid);
    }
    if (m_running.exchange(true)) {
        return;
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
    m_has_connection.store(false);
    m_current_ping_ms.store(-1.0);
}

void NetworkMonitor::set_target_pid(uint32_t target_pid) {
    m_target_pid.store(target_pid);
    if (target_pid == 0) {
        m_has_connection.store(false);
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

bool NetworkMonitor::has_valid_connection() const noexcept {
    return m_has_connection.load();
}

std::string NetworkMonitor::get_server_ip() const {
    std::lock_guard<std::mutex> lock(m_ip_mutex);
    return m_server_ip;
}

void NetworkMonitor::set_on_ping_update(std::function<void(double)> cb) {
    std::lock_guard<std::mutex> lock(m_callback_mutex);
    m_on_ping_update = std::move(cb);
}

void NetworkMonitor::set_mock_ping(double ping_ms) noexcept {
    m_current_ping_ms.store(ping_ms);
    m_has_connection.store(ping_ms >= 0.0);
    std::function<void(double)> cb;
    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        cb = m_on_ping_update;
    }
    if (cb && ping_ms >= 0.0) {
        cb(ping_ms);
    }
}

void NetworkMonitor::worker_loop() {
    while (m_running.load()) {
        probe_once();

        const uint32_t interval = m_probe_interval_ms.load();
        const uint32_t step = 50;
        for (uint32_t waited = 0; waited < interval && m_running.load(); waited += step) {
            std::this_thread::sleep_for(std::chrono::milliseconds(step));
        }
    }
}

bool NetworkMonitor::probe_once() {
    const uint32_t pid = m_target_pid.load();
    if (pid == 0) {
        m_has_connection.store(false);
        m_current_ping_ms.store(-1.0);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_ip_mutex);
        m_server_ip = "204.2.229.10"; // Simulated FFXIV NA Datacenter IP
    }
    m_has_connection.store(true);

    // If a mock ping was manually set, maintain it, otherwise default to 32.0ms mock
    double current = m_current_ping_ms.load();
    if (current < 0.0) {
        current = 32.0;
        m_current_ping_ms.store(current);
    }

    std::function<void(double)> cb;
    {
        std::lock_guard<std::mutex> lock(m_callback_mutex);
        cb = m_on_ping_update;
    }
    if (cb) {
        cb(current);
    }

    return true;
}

} // namespace hub::os

#endif
