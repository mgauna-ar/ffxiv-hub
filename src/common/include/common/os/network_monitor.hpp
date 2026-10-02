#pragma once

#include "common/os/ping_target.hpp"

#include <cstdint>
#include <atomic>
#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <thread>

namespace hub::os {

/**
 * @brief Background network ping prober.
 *
 * Finds the game server among the target FFXIV process's TCP connections and
 * pings it with an ICMP echo once a second, through the Windows IP Helper API. A
 * gaming VPN hides or rewrites those connections, so when none is visible, or the
 * server does not answer, it pings the lobby server of the data center the
 * character is on instead (see ping_candidates()). Runs in the desktop app process,
 * independent of the in-game payload's hooks, so it reports a ping on login before
 * any combat action occurs.
 */
class NetworkMonitor {
public:
    explicit NetworkMonitor(uint32_t target_pid = 0);
    ~NetworkMonitor();

    NetworkMonitor(const NetworkMonitor&) = delete;
    NetworkMonitor& operator=(const NetworkMonitor&) = delete;

    // The worker thread holds `this`, so the monitor never moves.
    NetworkMonitor(NetworkMonitor&&) = delete;
    NetworkMonitor& operator=(NetworkMonitor&&) = delete;

    /// Starts the background ping prober thread for the given PID.
    void start(uint32_t target_pid = 0);

    /// Stops the background prober thread.
    void stop();

    /// Updates the target PID (e.g. when game restarts or attaches).
    void set_target_pid(uint32_t target_pid);

    /// Returns the current target PID.
    [[nodiscard]] uint32_t target_pid() const noexcept;

    /// The character's current world, which the payload reports; 0 when unknown.
    /// Picks the data center lobby pinged when no game server answers. A new
    /// target PID clears it.
    void set_fallback_world(uint16_t world) noexcept { m_fallback_world.store(world); }
    [[nodiscard]] uint16_t fallback_world() const noexcept { return m_fallback_world.load(); }

    /// Returns the most recently measured network ping in milliseconds (-1.0 if timeout/NA).
    [[nodiscard]] double get_current_ping_ms() const noexcept;

    /// Returns whether the monitor prober thread is actively running.
    [[nodiscard]] bool is_running() const noexcept;

    /// Probe interval in milliseconds (default: 1000ms).
    void set_probe_interval_ms(uint32_t ms) noexcept { m_probe_interval_ms.store(ms); }
    [[nodiscard]] uint32_t probe_interval_ms() const noexcept { return m_probe_interval_ms.load(); }

    /// Helper for test injection: manually set ping value in mocks.
    void set_mock_ping(double ping_ms) noexcept;

private:
    void worker_loop();
    /// One ping of `pid`'s game server; the platform part. `pid` is never 0.
    bool probe_target(uint32_t pid);
    /// Logs what this probe pinged and whether it answered, when that changed.
    /// `answered` is null when nothing replied. Worker thread only.
    void note_outcome(std::span<const PingTarget> candidates, const PingTarget* answered, uint16_t world);

    std::atomic<bool> m_running{false};
    std::atomic<uint32_t> m_target_pid{0};
    std::atomic<uint32_t> m_probe_interval_ms{1000};
    std::atomic<double> m_current_ping_ms{-1.0};
    std::atomic<uint16_t> m_fallback_world{0};

    // note_outcome()'s state; worker thread only.
    std::string m_logged_outcome;
    uint32_t m_silent_probes{0};

    std::unique_ptr<std::thread> m_worker_thread;
};

} // namespace hub::os
