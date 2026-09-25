#include "meter/timeline.hpp"
#include <algorithm>

namespace hub::meter {

namespace {

/// Each value the average of the `window` values centred on it that exist.
std::vector<double> box_average(const std::vector<double>& values, size_t window) {
    const size_t n = values.size();
    std::vector<double> prefix(n + 1, 0.0);
    for (size_t i = 0; i < n; ++i) prefix[i + 1] = prefix[i] + values[i];
    std::vector<double> averaged(n);
    for (size_t i = 0; i < n; ++i) {
        const size_t lo = i >= window / 2 ? i - window / 2 : 0;
        const size_t hi = std::min(n - 1, i + (window - 1) / 2);
        averaged[i] = (prefix[hi + 1] - prefix[lo]) / static_cast<double>(hi - lo + 1);
    }
    return averaged;
}

} // namespace

double timeline_value(const TimelineBin& bin, TimelineMetric metric, DpsMetric dps) noexcept {
    switch (metric) {
        case TimelineMetric::Healing: return bin.healing;
        case TimelineMetric::Taken: return bin.taken;
        default:
            return metric_damage(dps, bin.damage, bin.buff_received, bin.buff_received_single, bin.buff_given);
    }
}

std::vector<float> smoothed_series(const std::vector<TimelineBin>& bins, size_t length,
                                   TimelineMetric metric, DpsMetric dps, size_t window_s) {
    std::vector<double> values(length, 0.0);
    for (size_t i = 0; i < length && i < bins.size(); ++i) {
        values[i] = timeline_value(bins[i], metric, dps);
    }
    // Two passes of half the window each weigh a second by how near the middle it
    // is, so a hit slides out of the average rather than dropping out of it at once.
    const size_t window = std::max<size_t>(window_s, 1);
    const size_t first = (window + 1) / 2;
    const std::vector<double> smoothed = box_average(box_average(values, first), window + 1 - first);
    return std::vector<float>(smoothed.begin(), smoothed.end());
}

std::vector<float> downsample(const std::vector<float>& values, size_t points) {
    if (points == 0) return {};
    if (values.size() <= points) return values;
    const size_t n = values.size();
    std::vector<float> out(points);
    for (size_t p = 0; p < points; ++p) {
        const size_t from = p * n / points;
        const size_t to = std::max(from + 1, (p + 1) * n / points);
        double sum = 0.0;
        for (size_t i = from; i < to; ++i) sum += values[i];
        out[p] = static_cast<float>(sum / static_cast<double>(to - from));
    }
    return out;
}

} // namespace hub::meter
