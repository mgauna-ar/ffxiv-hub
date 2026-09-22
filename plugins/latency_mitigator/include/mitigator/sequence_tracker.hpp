#pragma once

#include "mitigator/types.hpp"
#include <deque>
#include <optional>
#include <mutex>
#include <chrono>

namespace hub::mitigator {

class SequenceTracker {
public:
    explicit SequenceTracker(std::chrono::milliseconds stale_timeout = constants::DEFAULT_STALE_TIMEOUT);

    void record_request(
        ActionId action_id,
        SequenceId sequence,
        TimePoint timestamp = std::chrono::steady_clock::now(),
        bool is_cast = false,
        float cast_duration_seconds = 0.0f
    );

    [[nodiscard]] std::optional<ActionRequestInfo> match_response(
        ActionId action_id,
        SequenceId sequence,
        TimePoint timestamp = std::chrono::steady_clock::now()
    );

    size_t prune_stale(TimePoint now = std::chrono::steady_clock::now());
    [[nodiscard]] size_t pending_count() const;
    void clear();
    void set_stale_timeout(std::chrono::milliseconds timeout);

private:
    mutable std::mutex m_mutex;
    std::deque<ActionRequestInfo> m_pending;
    std::chrono::milliseconds m_stale_timeout{5000};
    static constexpr size_t MAX_PENDING_ENTRIES = 256;
};

} // namespace hub::mitigator
