#include "mitigator/rolling_rtt.hpp"
#include <algorithm>
#include <array>
#include <numeric>
#include <cmath>

namespace hub::mitigator {

namespace {
    constexpr double EMA_SMOOTHING_FACTOR = 2.0;

    bool is_plausible(double rtt_ms) {
        return rtt_ms >= constants::MIN_PLAUSIBLE_RTT_MS && rtt_ms <= constants::MAX_PLAUSIBLE_RTT_MS;
    }
}

RollingRttTracker::RollingRttTracker(size_t window_size, double initial_rtt_ms)
    : m_window_size(window_size > 0 ? window_size : constants::DEFAULT_RTT_SAMPLE_WINDOW),
      m_smoothed_rtt(initial_rtt_ms > 0.0 ? initial_rtt_ms : constants::DEFAULT_INITIAL_RTT_MS) {}

void RollingRttTracker::add_sample(double window_ms, double smoothed_ms) {
    if (!is_plausible(window_ms) || !is_plausible(smoothed_ms)) {
        return;
    }
    const double rtt_ms = smoothed_ms;

    std::lock_guard<std::mutex> lock(m_mutex);

    m_samples.push_back(window_ms);
    if (m_samples.size() > m_window_size) {
        m_samples.pop_front();
    }

    ++m_total_samples;

    const double n = static_cast<double>(std::min(m_total_samples, m_window_size));
    const double alpha = EMA_SMOOTHING_FACTOR / (n + 1.0);

    if (m_total_samples == 1) {
        m_smoothed_rtt = rtt_ms;
        m_jitter = 0.0;
    } else {
        const double diff = std::abs(rtt_ms - m_smoothed_rtt);
        m_smoothed_rtt = alpha * rtt_ms + (1.0 - alpha) * m_smoothed_rtt;
        m_jitter = alpha * diff + (1.0 - alpha) * m_jitter;
    }
}

double RollingRttTracker::get_smoothed_rtt_ms() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_smoothed_rtt;
}

double RollingRttTracker::get_median_rtt_ms() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_samples.empty()) {
        return m_smoothed_rtt;
    }

    const size_t n = m_samples.size();
    constexpr size_t STACK_BUFFER_SIZE = 64;

    if (n <= STACK_BUFFER_SIZE) {
        std::array<double, STACK_BUFFER_SIZE> stack_buf;
        std::copy(m_samples.begin(), m_samples.end(), stack_buf.begin());
        std::sort(stack_buf.begin(), stack_buf.begin() + n);

        const size_t mid = n / 2;
        if (n % 2 == 0) {
            return (stack_buf[mid - 1] + stack_buf[mid]) / 2.0;
        }
        return stack_buf[mid];
    }

    std::vector<double> sorted(m_samples.begin(), m_samples.end());
    std::sort(sorted.begin(), sorted.end());

    const size_t mid = sorted.size() / 2;
    if (sorted.size() % 2 == 0) {
        return (sorted[mid - 1] + sorted[mid]) / 2.0;
    }
    return sorted[mid];
}

double RollingRttTracker::get_average_rtt_ms() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_samples.empty()) {
        return m_smoothed_rtt;
    }
    const double sum = std::accumulate(m_samples.begin(), m_samples.end(), 0.0);
    return sum / static_cast<double>(m_samples.size());
}

double RollingRttTracker::get_jitter_ms() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_jitter;
}

size_t RollingRttTracker::sample_count() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_total_samples;
}

std::vector<double> RollingRttTracker::get_samples() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return std::vector<double>(m_samples.begin(), m_samples.end());
}

void RollingRttTracker::set_window_size(size_t window_size) {
    if (window_size == 0) return;
    std::lock_guard<std::mutex> lock(m_mutex);
    m_window_size = window_size;
    while (m_samples.size() > m_window_size) {
        m_samples.pop_front();
    }
}

void RollingRttTracker::reset(double initial_rtt_ms) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_samples.clear();
    m_smoothed_rtt = (initial_rtt_ms > 0.0) ? initial_rtt_ms : constants::DEFAULT_INITIAL_RTT_MS;
    m_jitter = 0.0;
    m_total_samples = 0;
}

} // namespace hub::mitigator
