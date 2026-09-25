#include "meter/gcd_uptime.hpp"
#include <algorithm>
#include <cstddef>
#include <vector>

namespace hub::meter {

namespace {

/// The recast the Action sheet gives a standard GCD, before speed shortens it.
constexpr double kBaseGcdS = 2.5;
/// Gaps, rescaled to a 2.5 s GCD, outside this range say nothing about the GCD: a
/// shorter one comes from a mudra or a dance step, a longer one from a delay.
constexpr double kMinGapS = 1.5;
constexpr double kMaxGapS = 3.6;
/// With fewer usable gaps the estimate stays at kBaseGcdS.
constexpr size_t kMinGaps = 8;
/// Low in the spread: a late press only ever lengthens a gap.
constexpr double kEstimatePercentile = 0.25;
/// How far a gap may run past its GCD and still count as rolling, for network jitter.
constexpr double kToleranceS = 0.1;
/// Haste (Ley Lines, Presence of Mind) brings a GCD back at most this much sooner.
constexpr double kHasteFloor = 0.75;

struct Gcd {
    double at_s{0.0};        // Since the pull started
    uint16_t recast_100ms{0};
    uint16_t cast_100ms{0};
};

double seconds(uint16_t tenths, double speed) {
    return static_cast<double>(tenths) * 0.1 * speed;
}

/// The 25th percentile of the gaps between two casts that land the same way, both
/// instant or both with one cast time, so where each lands in its GCD cancels out.
double estimate_gcd(const std::vector<Gcd>& gcds) {
    std::vector<double> gaps;
    gaps.reserve(gcds.size());
    for (size_t i = 1; i < gcds.size(); ++i) {
        const Gcd& prev = gcds[i - 1];
        if (gcds[i].cast_100ms != prev.cast_100ms) continue;
        const double gap = (gcds[i].at_s - prev.at_s) * kBaseGcdS / seconds(prev.recast_100ms, 1.0);
        if (gap >= kMinGapS && gap <= kMaxGapS) gaps.push_back(gap);
    }
    if (gaps.size() < kMinGaps) {
        return kBaseGcdS;
    }
    const auto nth = gaps.begin()
        + static_cast<std::ptrdiff_t>(kEstimatePercentile * static_cast<double>(gaps.size() - 1));
    std::nth_element(gaps.begin(), nth, gaps.end());
    return *nth;
}

} // namespace

GcdUptime gcd_uptime(std::span<const GcdCast> casts, uint64_t start_us, uint64_t end_us) {
    GcdUptime result;
    std::vector<Gcd> gcds;
    gcds.reserve(casts.size());
    for (const GcdCast& cast : casts) {
        if (cast.recast_100ms == 0) continue;
        const auto since_start = static_cast<int64_t>(cast.at_us - start_us);
        gcds.push_back(Gcd{static_cast<double>(since_start) * 1e-6, cast.recast_100ms, cast.cast_100ms});
    }
    if (gcds.empty() || end_us <= start_us) {
        return result;
    }
    result.estimate_s = estimate_gcd(gcds);
    const double speed = result.estimate_s / kBaseGcdS;

    // When each GCD was pressed and how long it held the GCD. An instant lands as it
    // is pressed, a hardcast its cast time later.
    std::vector<double> pressed(gcds.size());
    std::vector<double> held(gcds.size());
    for (size_t i = 0; i < gcds.size(); ++i) {
        const Gcd& gcd = gcds[i];
        const double recast = seconds(gcd.recast_100ms, speed);
        const double cast = seconds(gcd.cast_100ms, speed);
        pressed[i] = gcd.at_s;
        held[i] = recast;
        if (cast <= 0.0) continue;
        const double hardcast = gcd.at_s - cast;
        // A hardcast started once the previous GCD was back and its cast was over.
        const bool fits = i == 0 || hardcast >= std::max(
            pressed[i - 1] + seconds(gcds[i - 1].recast_100ms, speed) * kHasteFloor,
            gcds[i - 1].at_s) - kToleranceS;
        if (fits) {
            pressed[i] = hardcast;
            held[i] = std::max(recast, cast);
        }
    }

    // Each GCD rolls until the next press, or for its own length and a little slack when
    // the next one came late. The pull's edges clip it.
    const double end_s = static_cast<double>(end_us - start_us) * 1e-6;
    double covered = 0.0;
    for (size_t i = 0; i < gcds.size(); ++i) {
        const double rolling = i + 1 < gcds.size()
            ? std::min(pressed[i + 1] - pressed[i], held[i] + kToleranceS)
            : held[i];
        const double from = std::max(pressed[i], 0.0);
        const double to = std::min(pressed[i] + rolling, end_s);
        if (to > from) covered += to - from;
    }
    result.uptime_pct = std::clamp(covered * 100.0 / end_s, 0.0, 100.0);
    return result;
}

} // namespace hub::meter
