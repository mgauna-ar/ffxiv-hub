#pragma once

#include "meter/types.hpp"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hub::meter {

/// Longest a timeline runs: a pull past an hour keeps its first hour.
constexpr size_t TIMELINE_MAX_SECONDS = 3600;

/// What one party member did in one second of a pull.
struct TimelineBin {
    uint32_t damage{0};
    uint32_t healing{0};              // Effective: overheal is left out
    uint32_t taken{0};
    uint32_t buff_given{0};           // What this player's buffs added to other players' hits
    uint32_t buff_received{0};        // What other players' buffs added to this player's hits
    uint32_t buff_received_single{0}; // The part of buff_received from cards and dance partner
};
static_assert(sizeof(TimelineBin) == 24, "A timeline's memory is sized by this");

/// One party member's pull: bins[i] is second i from the pull's start.
struct TimelineRow {
    EntityId entity{0};
    std::vector<TimelineBin> bins;
};

/// A party-wide raid buff one source kept up, merged over everyone it was on.
struct BuffWindow {
    uint16_t status{0};
    EntityId source{0};                   // 0 when unknown
    double begin_s{0.0};                  // Since the pull started
    double end_s{0.0};
};

/// A pull second by second, for the Timeline tab.
struct EncounterTimeline {
    std::vector<TimelineRow> rows;        // By entity id
    std::vector<BuffWindow> buffs;        // By start
};

/// What a timeline line shows.
enum class TimelineMetric : uint8_t {
    Damage,  // As the DpsMetric picked counts it
    Healing,
    Taken,
};

/// One second's value of `metric`.
[[nodiscard]] double timeline_value(const TimelineBin& bin, TimelineMetric metric, DpsMetric dps) noexcept;

/// `metric` for each of `length` seconds, each an average of the `window_s` seconds
/// centred on it that fall inside the pull, weighted towards the middle (1, 2, 3, 2, 1
/// for five). Seconds past the last bin count as 0.
[[nodiscard]] std::vector<float> smoothed_series(const std::vector<TimelineBin>& bins, size_t length,
                                                 TimelineMetric metric, DpsMetric dps, size_t window_s);

/// `values` averaged down to at most `points` points, one per equal span.
[[nodiscard]] std::vector<float> downsample(const std::vector<float>& values, size_t points);

} // namespace hub::meter
