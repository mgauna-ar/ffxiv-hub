#include "meter/downtime.hpp"
#include <algorithm>

namespace hub::meter {

void DowntimeTracker::start(uint64_t start_us) {
    clear();
    m_start_us = start_us;
}

void DowntimeTracker::observe(bool targetable, uint64_t timestamp_us) {
    if (timestamp_us < m_start_us) {
        return;
    }
    if (!targetable) {
        if (!m_open_since) m_open_since = timestamp_us;
        return;
    }
    if (m_open_since) {
        if (timestamp_us > *m_open_since) m_closed.emplace_back(*m_open_since, timestamp_us);
        m_open_since.reset();
    }
}

double DowntimeTracker::seconds_until(uint64_t end_us) const {
    // A pull's clock can stop before a stretch ends, or before one begins: a kill
    // ends it at the last hit, and the dead boss reads untargetable after that.
    uint64_t total_us = 0;
    const auto add = [&](uint64_t from, uint64_t to) {
        to = std::min(to, end_us);
        if (to > from) total_us += to - from;
    };
    for (const auto& [from, to] : m_closed) add(from, to);
    if (m_open_since) add(*m_open_since, end_us);
    return static_cast<double>(total_us) / 1e6;
}

void DowntimeTracker::clear() {
    m_start_us = 0;
    m_open_since.reset();
    m_closed.clear();
}

} // namespace hub::meter
