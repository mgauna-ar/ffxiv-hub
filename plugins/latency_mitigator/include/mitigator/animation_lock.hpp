#pragma once

#include "mitigator/types.hpp"
#include "mitigator/rolling_rtt.hpp"
#include "mitigator/sequence_tracker.hpp"
#include "mitigator/cast_tracker.hpp"
#include <mutex>

namespace hub::mitigator {

class AnimationLockMitigator {
public:
    explicit AnimationLockMitigator(const MitigationConfig& config = {});

    void record_action_request(
        ActionId action_id,
        SequenceId sequence,
        TimePoint timestamp = std::chrono::steady_clock::now(),
        bool is_cast = false,
        float cast_duration_seconds = 0.0f,
        bool is_queued = false
    );

    [[nodiscard]] MitigationResult calculate_mitigation(
        ActionId action_id,
        SequenceId sequence,
        double original_lock_ms,
        TimePoint now = std::chrono::steady_clock::now()
    );

    void record_cast_begin(ActionId action_id, float cast_time_seconds, TimePoint now = std::chrono::steady_clock::now());
    void record_cast_interrupt(TimePoint now = std::chrono::steady_clock::now());
    void record_cast_end(TimePoint now = std::chrono::steady_clock::now());

    [[nodiscard]] bool is_casting(TimePoint now = std::chrono::steady_clock::now()) const;
    [[nodiscard]] MitigationConfig get_config() const;
    void set_config(const MitigationConfig& config);
    void set_dry_run(bool dry_run);
    void set_enabled(bool enabled);
    void set_target_ping_ms(double target_ping_ms);
    void set_min_animation_lock_ms(double min_lock_ms);
    void set_spike_multiplier(double multiplier);

    [[nodiscard]] SessionStats get_session_stats() const;
    [[nodiscard]] const RollingRttTracker& get_rtt_tracker() const noexcept { return m_rtt_tracker; }
    [[nodiscard]] const SequenceTracker& get_sequence_tracker() const noexcept { return m_seq_tracker; }

    void reset();

private:
    mutable std::mutex     m_mutex;
    MitigationConfig       m_config;
    RollingRttTracker      m_rtt_tracker;
    SequenceTracker        m_seq_tracker;
    CastTracker            m_cast_tracker;

    uint64_t m_total_actions_requested{0};
    uint64_t m_total_actions_mitigated{0};
    double   m_cumulative_time_saved_ms{0.0};
    uint64_t m_total_floor_clamps{0};
};

} // namespace hub::mitigator
