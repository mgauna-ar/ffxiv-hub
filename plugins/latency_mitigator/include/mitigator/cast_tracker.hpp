#pragma once

#include "mitigator/types.hpp"
#include <mutex>
#include <chrono>

namespace hub::mitigator {

class CastTracker {
public:
    CastTracker() = default;

    void on_cast_begin(ActionId action_id, float cast_time_seconds, TimePoint now = std::chrono::steady_clock::now());
    void on_cast_interrupt(TimePoint now = std::chrono::steady_clock::now());
    void on_cast_end(TimePoint now = std::chrono::steady_clock::now());

    [[nodiscard]] bool is_casting(TimePoint now = std::chrono::steady_clock::now(), double smoothed_rtt_ms = 0.0) const;
    [[nodiscard]] ActionId current_cast_action_id() const;
    [[nodiscard]] float remaining_cast_time_seconds(TimePoint now = std::chrono::steady_clock::now()) const;

    void reset();

private:
    mutable std::mutex m_mutex;
    mutable bool       m_is_casting{false};
    mutable ActionId   m_cast_action_id{0};
    TimePoint          m_cast_start{std::chrono::steady_clock::now()};
    mutable float      m_cast_duration_seconds{0.0f};
};

} // namespace hub::mitigator
