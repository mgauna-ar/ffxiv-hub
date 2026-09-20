#include "mitigator/cast_tracker.hpp"
#include <algorithm>

namespace hub::mitigator {

void CastTracker::on_cast_begin(ActionId action_id, float cast_time_seconds, TimePoint now) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_is_casting = true;
    m_cast_action_id = action_id;
    m_cast_duration_seconds = std::max(0.0f, cast_time_seconds);
    m_cast_start = now;
}

void CastTracker::on_cast_interrupt(TimePoint /*now*/) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_is_casting = false;
    m_cast_action_id = 0;
    m_cast_duration_seconds = 0.0f;
}

void CastTracker::on_cast_end(TimePoint /*now*/) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_is_casting = false;
    m_cast_action_id = 0;
    m_cast_duration_seconds = 0.0f;
}

bool CastTracker::is_casting(TimePoint now, double smoothed_rtt_ms) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_is_casting) {
        return false;
    }

    const auto elapsed = std::chrono::duration<float>(now - m_cast_start).count();
    if (elapsed < 0.0f) {
        return false;
    }

    if (elapsed >= constants::ABSOLUTE_MAX_CAST_DURATION_SECONDS) {
        m_is_casting = false;
        m_cast_action_id = 0;
        m_cast_duration_seconds = 0.0f;
        return false;
    }

    const float dynamic_grace = std::max(
        constants::CAST_COMPLETION_GRACE_WINDOW_SECONDS,
        static_cast<float>((smoothed_rtt_ms * constants::ONE_WAY_LATENCY_RATIO) / constants::MS_PER_SECOND)
            + constants::CAST_GRACE_BASE_BUFFER_SECONDS
    );
    return elapsed < (m_cast_duration_seconds + dynamic_grace);
}

ActionId CastTracker::current_cast_action_id() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_is_casting) {
        return 0;
    }
    return m_cast_action_id;
}

float CastTracker::remaining_cast_time_seconds(TimePoint now) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_is_casting) {
        return 0.0f;
    }

    const auto elapsed = std::chrono::duration<float>(now - m_cast_start).count();
    if (elapsed < 0.0f || elapsed >= m_cast_duration_seconds) {
        return 0.0f;
    }
    return m_cast_duration_seconds - elapsed;
}

void CastTracker::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_is_casting = false;
    m_cast_action_id = 0;
    m_cast_duration_seconds = 0.0f;
}

} // namespace hub::mitigator
