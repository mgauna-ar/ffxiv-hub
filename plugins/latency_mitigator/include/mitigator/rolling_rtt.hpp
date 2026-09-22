#pragma once

#include "mitigator/types.hpp"
#include <vector>
#include <deque>
#include <mutex>
#include <cstddef>

namespace hub::mitigator {

class RollingRttTracker {
public:
    explicit RollingRttTracker(size_t window_size = 10, double initial_rtt_ms = 50.0);

    /// Records one round trip. The median window keeps `window_ms` as measured, so
    /// a sustained shift moves the median within about half a window; the EMA and
    /// jitter follow `smoothed_ms`, which the caller has already spike-filtered.
    void add_sample(double window_ms, double smoothed_ms);
    void add_sample(double rtt_ms) { add_sample(rtt_ms, rtt_ms); }

    [[nodiscard]] double get_smoothed_rtt_ms() const;
    [[nodiscard]] double get_median_rtt_ms() const;
    [[nodiscard]] double get_average_rtt_ms() const;
    [[nodiscard]] double get_jitter_ms() const;
    [[nodiscard]] size_t sample_count() const;
    [[nodiscard]] std::vector<double> get_samples() const;

    void set_window_size(size_t window_size);
    void reset(double initial_rtt_ms = 50.0);

private:
    mutable std::mutex m_mutex;
    std::deque<double> m_samples;
    size_t m_window_size{10};
    double m_smoothed_rtt{50.0};
    double m_jitter{0.0};
    size_t m_total_samples{0};
};

} // namespace hub::mitigator
