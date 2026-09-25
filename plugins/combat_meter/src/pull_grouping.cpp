#include "meter/pull_grouping.hpp"
#include "meter/types.hpp"

namespace hub::meter {

std::vector<PullGroup> group_pulls_by_visit(const std::vector<PullHistoryEntry>& pulls) {
    std::vector<PullGroup> groups;

    // Walk newest to oldest so both the groups and the pulls inside them come
    // out in that order without a second sort. Visit ids only grow and pulls are
    // archived in order, so each visit is one run.
    for (size_t i = pulls.size(); i-- > 0;) {
        const PullHistoryEntry& pull = pulls[i];
        if (groups.empty() || groups.back().zone_visit != pull.zone_visit) {
            std::string label = zone_label(pull.zone_id, pull.zone_name);
            if (label.empty()) label = "Unknown zone";
            groups.push_back(PullGroup{pull.zone_visit, pull.zone_id, std::move(label), {}});
        }
        groups.back().pulls.push_back(i);
    }

    return groups;
}

} // namespace hub::meter
