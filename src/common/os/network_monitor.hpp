#pragma once

#include <cstdint>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace hub::os {

/**
 * @brief Background network ping prober.
 *
 * Automatically inspects the active TCP connections of the target FFXIV process,
 * extracts the remote game server IP, and conducts periodic ICMP Echo pings (1/sec)
 * via native Windows IP Helper APIs without game hooks or extra network overhead.
 * Runs in the desktop app process, independent of the in-game payload, so it can
 * report a connection ping immediately on login before any combat action occurs.
 */
class NetworkMonitor {
public:
    explicit NetworkMonitor(uint32_t target_pid = 0);
    ~NetworkMonitor();

    NetworkMonitor(const NetworkMonitor&) = delete;
    NetworkMonitor& operator=(const NetworkMonitor&) = delete;

    NetworkMonitor(NetworkMonitor&& other) noexcept;
    NetworkMonitor& operator=(NetworkMonitor&& other) noexcept;

    /// Starts the background ping prober thread for the given PID.
    void start(uint32_t target_pid = 0);

    /// Stops the background prober thread.
    void stop();

    /// Updates the target PID (e.g. when game restarts or attaches).
    void set_target_pid(uint32_t target_pid);

    /// Returns the current target PID.
    [[nodiscard]] uint32_t target_pid() const noexcept;

    /// Returns the most recently measured network ping in milliseconds (-1.0 if timeout/NA).
    [[nodiscard]] double get_current_ping_ms() const noexcept;

    /// Returns whether the monitor prober thread is actively running.
    [[nodiscard]] bool is_running() const noexcept;

    /// Returns whether an active game server connection was detected.
    [[nodiscard]] bool has_valid_connection() const noexcept;

    /// Returns the string representation of the detected server IP (e.g. "204.2.229.10").
    [[nodiscard]] std::string get_server_ip() const;

    /// Sets an optional callback fired whenever a new ping sample is measured.
    void set_on_ping_update(std::function<void(double)> cb);

    /// Probe interval in milliseconds (default: 1000ms).
    void set_probe_interval_ms(uint32_t ms) noexcept { m_probe_interval_ms.store(ms); }
    [[nodiscard]] uint32_t probe_interval_ms() const noexcept { return m_probe_interval_ms.load(); }

    /// Helper for test injection: manually set ping value in mocks.
    void set_mock_ping(double ping_ms) noexcept;

private:
    void worker_loop();
    bool probe_once();

    std::atomic<bool> m_running{false};
    std::atomic<uint32_t> m_target_pid{0};
    std::atomic<uint32_t> m_probe_interval_ms{1000};
    std::atomic<double> m_current_ping_ms{-1.0};
    std::atomic<bool> m_has_connection{false};

    mutable std::mutex m_ip_mutex;
    std::string m_server_ip;

    mutable std::mutex m_callback_mutex;
    std::function<void(double)> m_on_ping_update;

    std::unique_ptr<std::thread> m_worker_thread;
};

} // namespace hub::os
