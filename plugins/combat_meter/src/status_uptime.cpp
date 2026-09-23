#include "meter/status_uptime.hpp"
#include "hub/game/status.hpp"
#include <algorithm>

namespace hub::meter {

void StatusUptime::start(uint64_t start_us, const CombatantRegistry& registry) {
    m_tracks.clear();
    m_start_us = start_us;
    m_end_us = 0;
    m_running = true;
    for (const auto& [id, actor] : registry.all_actors()) {
        for (const ActiveStatus& status : actor.statuses) {
            open(Key{id, status.status_id, status.source}, start_us);
        }
    }
}

void StatusUptime::open(const Key& key, uint64_t at_us) {
    std::vector<Interval>& intervals = m_tracks[key];
    if (!intervals.empty() && intervals.back().end_us == kOpen) {
        return;
    }
    intervals.push_back(Interval{std::max(at_us, m_start_us), kOpen});
}

void StatusUptime::apply(const std::vector<StatusChange>& changes) {
    if (!m_running) {
        return;
    }
    for (const StatusChange& change : changes) {
        const Key key{change.target, change.status_id, change.source};
        if (change.gained) {
            open(key, change.at_us);
            continue;
        }
        auto it = m_tracks.find(key);
        if (it == m_tracks.end() || it->second.empty() || it->second.back().end_us != kOpen) {
            continue;
        }
        Interval& last = it->second.back();
        // Gone before the pull began (reported late): it was never part of this pull.
        if (change.at_us <= m_start_us && last.begin_us == m_start_us) {
            it->second.pop_back();
            continue;
        }
        last.end_us = std::max(change.at_us, last.begin_us);
    }
}

void StatusUptime::stop(uint64_t end_us) {
    if (!m_running) {
        return;
    }
    m_running = false;
    m_end_us = std::max(end_us, m_start_us);
}

std::vector<StatusUptimeRow> StatusUptime::rows(uint64_t now_us, const CombatantRegistry& registry) const {
    std::vector<StatusUptimeRow> rows;
    if (m_tracks.empty()) {
        return rows;
    }
    const uint64_t end_us = m_running ? std::max(now_us, m_start_us) : m_end_us;
    // Same floor as the rate math, so a pull that barely started cannot divide by zero.
    const double duration_s = std::max(static_cast<double>(end_us - m_start_us) / 1e6, 1.0);

    rows.reserve(m_tracks.size());
    for (const auto& [key, intervals] : m_tracks) {
        StatusUptimeRow row;
        row.target = key.target;
        row.status = key.status;
        row.source = key.source;
        uint64_t active_us = 0;
        for (const Interval& interval : intervals) {
            if (interval.begin_us > end_us) continue;
            row.applications++;
            const uint64_t until = std::min(interval.end_us, end_us);
            if (until > interval.begin_us) {
                active_us += until - interval.begin_us;
            }
        }
        if (row.applications == 0) continue;
        row.active_s = static_cast<double>(active_us) / 1e6;
        row.uptime_pct = std::min(row.active_s / duration_s * 100.0, 100.0);
        row.detrimental = hub::game::status_is_detrimental(key.status);
        row.on_enemy = !registry.is_friendly(key.target);
        rows.push_back(row);
    }
    std::sort(rows.begin(), rows.end(), [](const StatusUptimeRow& a, const StatusUptimeRow& b) {
        if (a.active_s != b.active_s) return a.active_s > b.active_s;
        if (a.target != b.target) return a.target < b.target;
        if (a.status != b.status) return a.status < b.status;
        return a.source < b.source;
    });
    return rows;
}

void StatusUptime::clear() {
    m_tracks.clear();
    m_start_us = 0;
    m_end_us = 0;
    m_running = false;
}

} // namespace hub::meter
