#pragma once

#include "meter/encounter_engine.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace hub::meter {

/// One zone's pulls, as the picker draws them.
struct PullGroup {
    uint32_t zone_id{0};
    std::string label;
    /// Indices into the vector passed to group_pulls_by_zone, newest first.
    std::vector<size_t> pulls;
};

/// Buckets an archive listing by zone for the pull picker.
///
/// Ordering follows archive position rather than ended_at_unix_s: that field is
/// 0 on pulls archived before it existed, and the history deque is already
/// chronological. Groups come back newest-zone-first, pulls newest-first inside
/// each, and revisiting a duty folds into its existing group.
[[nodiscard]] std::vector<PullGroup> group_pulls_by_zone(const std::vector<PullHistoryEntry>& pulls);

} // namespace hub::meter
