#include "meter/pull_grouping.hpp"
#include "meter/types.hpp"
#include <algorithm>
#include <unordered_map>

namespace hub::meter {

std::vector<PullGroup> group_pulls_by_zone(const std::vector<PullHistoryEntry>& pulls) {
    std::vector<PullGroup> groups;
    std::unordered_map<uint32_t, size_t> group_of_zone;

    // Walk newest to oldest so both the groups and the pulls inside them come
    // out in that order without a second sort.
    for (size_t i = pulls.size(); i-- > 0;) {
        const PullHistoryEntry& pull = pulls[i];
        auto [it, inserted] = group_of_zone.emplace(pull.zone_id, groups.size());
        if (inserted) {
            std::string label = zone_label(pull.zone_id, pull.zone_name);
            if (label.empty()) label = "Unknown zone";
            groups.push_back(PullGroup{pull.zone_id, std::move(label), {}});
        }
        groups[it->second].pulls.push_back(i);
    }

    return groups;
}

} // namespace hub::meter
