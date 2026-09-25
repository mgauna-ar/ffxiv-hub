#include "meter/status_uptime.hpp"
#include "hub/game/raid_buffs.hpp"
#include "hub/game/status.hpp"
#include <algorithm>
#include <map>
#include <utility>

namespace hub::meter {

namespace {

/// A gap this short between two runs of one buff is the same application seen at a
/// different moment on each target, not a new one.
constexpr uint64_t kWindowBridgeUs = 1'000'000;

} // namespace

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

std::vector<BuffWindow> StatusUptime::windows(uint64_t now_us, const CombatantRegistry& registry) const {
    std::vector<BuffWindow> windows;
    const uint64_t end_us = m_running ? std::max(now_us, m_start_us) : m_end_us;

    // Every target's intervals of one buff from one source, in one list.
    std::map<std::pair<uint16_t, EntityId>, std::vector<Interval>> runs;
    for (const auto& [key, intervals] : m_tracks) {
        const game::RaidBuff* buff = game::find_raid_buff(key.status);
        if (buff == nullptr || buff->single_target || buff->on_enemy == registry.is_friendly(key.target)) {
            continue;
        }
        std::vector<Interval>& run = runs[{key.status, key.source}];
        for (const Interval& interval : intervals) {
            const uint64_t until = std::min(interval.end_us, end_us);
            if (until > interval.begin_us) run.push_back(Interval{interval.begin_us, until});
        }
    }

    const auto seconds = [this](uint64_t us) { return static_cast<double>(us - m_start_us) / 1e6; };
    for (auto& [id, run] : runs) {
        if (run.empty()) continue;
        std::sort(run.begin(), run.end(), [](const Interval& a, const Interval& b) { return a.begin_us < b.begin_us; });
        Interval merged = run.front();
        const auto emit = [&] { windows.push_back(BuffWindow{id.first, id.second, seconds(merged.begin_us), seconds(merged.end_us)}); };
        for (size_t i = 1; i < run.size(); ++i) {
            if (run[i].begin_us <= merged.end_us + kWindowBridgeUs) {
                merged.end_us = std::max(merged.end_us, run[i].end_us);
            } else {
                emit();
                merged = run[i];
            }
        }
        emit();
    }
    std::sort(windows.begin(), windows.end(), [](const BuffWindow& a, const BuffWindow& b) {
        if (a.begin_s != b.begin_s) return a.begin_s < b.begin_s;
        if (a.status != b.status) return a.status < b.status;
        return a.source < b.source;
    });
    return windows;
}

void StatusUptime::clear() {
    m_tracks.clear();
    m_start_us = 0;
    m_end_us = 0;
    m_running = false;
}

} // namespace hub::meter
