#pragma once

#include <chrono>

namespace hub::payload {

/**
 * @brief One job of the orchestration loop that runs every `interval`.
 *
 * The loop polls it once per tick, so a job runs up to one tick late and never
 * catches up on missed periods. `due()` and `mark()` are separate for jobs that
 * also run on another trigger (the game state push runs on change as well) or only
 * under a condition (the heartbeat only while connected): those check `due()`,
 * decide, and `mark()` when they actually ran.
 */
class Periodic {
public:
    using Clock = std::chrono::steady_clock;

    /// `last` is when the job is taken to have last run: the loop's start delays
    /// the first run by one interval, `Clock::time_point{}` makes it run at once.
    constexpr Periodic(Clock::duration interval, Clock::time_point last) noexcept
        : m_interval(interval), m_last(last) {}

    [[nodiscard]] constexpr bool due(Clock::time_point now) const noexcept {
        return now - m_last >= m_interval;
    }

    constexpr void mark(Clock::time_point now) noexcept { m_last = now; }

    /// due(), and if so mark(): for a job with no other trigger or condition.
    [[nodiscard]] constexpr bool fire(Clock::time_point now) noexcept {
        if (!due(now)) return false;
        mark(now);
        return true;
    }

private:
    Clock::duration m_interval;
    Clock::time_point m_last;
};

} // namespace hub::payload
