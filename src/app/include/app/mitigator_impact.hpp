#pragma once

#include "common/ipc/protocol.hpp"

#include <cstddef>
#include <optional>
#include <span>

namespace hub::app {

/// How long one action kept the player from acting again, as the client feels it:
/// the server's lock only lands once its reply does, so the wait is the round trip
/// plus whatever lock was written then.
struct WeaveTiming {
    float rtt_ms{0.0f};
    float without_ms{0.0f}; ///< Round trip plus the lock the server sent.
    float with_ms{0.0f};    ///< Round trip plus the lock written (or, in dry-run, that would be).
    [[nodiscard]] float saved_ms() const { return without_ms - with_ms; }
};

/// The timing for one telemetry sample, or nothing for a sample that has no place
/// in a with/without comparison: a cast (never trimmed, by design), a reply that
/// matched no request (no round trip), or a lock above the ceiling, which passes
/// through untouched and would dwarf every other action on the chart.
[[nodiscard]] inline std::optional<WeaveTiming> weave_timing(const ipc::MitigatorTelemetryPayload& s,
                                                            float ceiling_ms) {
    if (s.cast_active != 0 || s.measured_rtt_ms <= 0.0f || s.original_lock_ms > ceiling_ms) {
        return std::nullopt;
    }
    WeaveTiming t{};
    t.rtt_ms = s.measured_rtt_ms;
    t.without_ms = s.measured_rtt_ms + s.original_lock_ms;
    // Switched off, adjusted equals original, so with and without agree. Dry-run
    // reports the lock it would have written, which is what "with" means there.
    t.with_ms = s.measured_rtt_ms + s.adjusted_lock_ms;
    return t;
}

/// Averages over the most recent comparable actions, for the Time saved tile.
struct ImpactSummary {
    std::size_t samples{0};
    float avg_without_ms{0.0f};
    float avg_with_ms{0.0f};
    [[nodiscard]] float avg_saved_ms() const { return avg_without_ms - avg_with_ms; }
};

/// Actions the tile averages over: recent enough to follow a change of route or
/// setting within a pull, long enough that one spike does not swing it.
inline constexpr std::size_t kImpactWindow = 30;

/// Averages the newest `window` samples that weave_timing() accepts. `recent` is
/// oldest first, as AppState::get_recent_telemetry returns it.
[[nodiscard]] inline ImpactSummary summarize_impact(std::span<const ipc::MitigatorTelemetryPayload> recent,
                                                    float ceiling_ms,
                                                    std::size_t window = kImpactWindow) {
    ImpactSummary out{};
    double without_sum = 0.0, with_sum = 0.0;
    for (auto it = recent.rbegin(); it != recent.rend() && out.samples < window; ++it) {
        const auto t = weave_timing(*it, ceiling_ms);
        if (!t) continue;
        without_sum += t->without_ms;
        with_sum += t->with_ms;
        ++out.samples;
    }
    if (out.samples == 0) return out;

    const double n = static_cast<double>(out.samples);
    out.avg_without_ms = static_cast<float>(without_sum / n);
    out.avg_with_ms = static_cast<float>(with_sum / n);
    return out;
}

} // namespace hub::app
