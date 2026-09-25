#pragma once

#include "meter/encounter_engine.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace hub::meter {

/// One zone visit's pulls, as the picker draws them.
struct PullGroup {
    uint32_t zone_visit{0};
    uint32_t zone_id{0};
    std::string label;
    /// Indices into the vector passed to group_pulls_by_visit, newest first.
    std::vector<size_t> pulls;
};

/// Buckets an archive listing by zone visit for the pull picker.
///
/// Each stay in a zone is its own group, so a duty entered twice is listed twice.
/// Ordering follows archive position rather than ended_at_unix_s: that field is
/// 0 on pulls archived before it existed, and the history deque is already
/// chronological. Groups come back newest-visit-first, pulls newest-first inside
/// each.
[[nodiscard]] std::vector<PullGroup> group_pulls_by_visit(const std::vector<PullHistoryEntry>& pulls);

} // namespace hub::meter
