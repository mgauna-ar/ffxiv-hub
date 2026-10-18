#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace hub::meter {

/// The stretches of a pull in which no enemy the vitals pass read could be hit:
/// every one untargetable or dead. A pull's damage rates leave them out.
///
/// Fed from CombatTargetability reports, which the payload sends when the answer
/// changes. A pull opens on a landed hit, so it opens with something to hit.
class DowntimeTracker {
public:
    /// A pull starting at `start_us`.
    void start(uint64_t start_us);

    /// Whether any tracked enemy could be hit as of `timestamp_us`. A report stamped
    /// before the pull's start describes the fight before it and is dropped.
    void observe(bool targetable, uint64_t timestamp_us);

    /// Seconds of downtime between the pull's start and `end_us`. A stretch still
    /// open counts up to `end_us`.
    [[nodiscard]] double seconds_until(uint64_t end_us) const;

    void clear();

private:
    uint64_t m_start_us{0};
    std::optional<uint64_t> m_open_since;
    /// Closed stretches, [from, to) in steady-clock microseconds.
    std::vector<std::pair<uint64_t, uint64_t>> m_closed;
};

} // namespace hub::meter
