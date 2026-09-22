#include "mitigator/animation_lock.hpp"
#include <algorithm>
#include <cmath>

namespace hub::mitigator {

namespace {

/// A hand-edited config can carry values that would cut the lock by more than the
/// measured round trip, or break the sample window. Every config path ends here.
MitigationConfig sanitize(MitigationConfig cfg) {
    cfg.target_ping_ms = std::max(0.0, cfg.target_ping_ms);
    cfg.safety_margin_ms = std::max(0.0, cfg.safety_margin_ms);
    cfg.spike_multiplier = std::max(1.0, cfg.spike_multiplier);
    cfg.min_animation_lock_ms = std::max(constants::ABSOLUTE_MIN_ANIMATION_LOCK_FLOOR_MS, cfg.min_animation_lock_ms);
    cfg.max_animation_lock_ms = std::max(cfg.min_animation_lock_ms, cfg.max_animation_lock_ms);
    cfg.rtt_sample_window = std::clamp<size_t>(cfg.rtt_sample_window, 1, constants::MAX_RTT_SAMPLE_WINDOW);
    return cfg;
}

} // namespace

AnimationLockMitigator::AnimationLockMitigator(const MitigationConfig& config)
    : m_config(sanitize(config)),
      m_rtt_tracker(m_config.rtt_sample_window, m_config.target_ping_ms > 0 ? m_config.target_ping_ms * 3.0 : 50.0) {}

void AnimationLockMitigator::record_action_request(
    ActionId action_id,
    SequenceId sequence,
    TimePoint timestamp,
    bool is_cast,
    float cast_duration_seconds,
    bool is_queued
) {
    std::lock_guard<std::mutex> lock(m_mutex);
    ++m_total_actions_requested;
    m_seq_tracker.record_request(action_id, sequence, timestamp, is_cast, cast_duration_seconds, is_queued);
}

MitigationResult AnimationLockMitigator::calculate_mitigation(
    ActionId action_id,
    SequenceId sequence,
    double original_lock_ms,
    TimePoint now
) {
    std::lock_guard<std::mutex> lock(m_mutex);

    MitigationResult res{};
    res.action_id = action_id;
    res.sequence = sequence;
    res.original_lock_ms = original_lock_ms;

    const auto matched_req = m_seq_tracker.match_response(action_id, sequence, now);
    if (!matched_req.has_value()) {
        res.adjusted_lock_ms = original_lock_ms;
        res.delay_reduced_ms = 0.0;
        res.applied = false;
        res.measured_rtt_ms = 0.0;
        res.smoothed_rtt_ms = m_rtt_tracker.get_smoothed_rtt_ms();
        return res;
    }

    const bool is_cast_effect = matched_req->is_cast;
    const bool is_currently_casting = m_cast_tracker.is_casting(now, m_rtt_tracker.get_smoothed_rtt_ms());
    res.cast_active = is_cast_effect || is_currently_casting;

    if (res.cast_active) {
        if (is_cast_effect) {
            m_cast_tracker.on_cast_end(now);
        }
        res.adjusted_lock_ms = original_lock_ms;
        res.delay_reduced_ms = 0.0;
        res.applied = false;
        res.measured_rtt_ms = 0.0;
        res.smoothed_rtt_ms = m_rtt_tracker.get_smoothed_rtt_ms();
        return res;
    }

    const double baseline_rtt = m_rtt_tracker.get_smoothed_rtt_ms();
    const size_t samples_before = m_rtt_tracker.sample_count();

    double measured_rtt = 0.0;
    const auto elapsed = std::chrono::duration_cast<Milliseconds>(
        now - matched_req->timestamp
    ).count();

    double effective_rtt = baseline_rtt;

    if (elapsed > 0.0 && elapsed < constants::MAX_PLAUSIBLE_RTT_MS) {
        measured_rtt = elapsed;
        // A queued request was stamped at key-press, so its elapsed time includes the
        // wait in the queue: an upper bound on the round trip, never a measurement of it.
        double window_sample = matched_req->is_queued
            ? std::min(measured_rtt, m_rtt_tracker.get_median_rtt_ms())
            : measured_rtt;
        effective_rtt = window_sample;

        if (samples_before >= constants::MIN_SAMPLES_FOR_MEDIAN_FILTER) {
            const double median_rtt = m_rtt_tracker.get_median_rtt_ms();
            const double jitter = m_rtt_tracker.get_jitter_ms();
            const double outlier_tolerance = std::max({
                constants::MIN_OUTLIER_TOLERANCE_MS,
                median_rtt * 0.5,
                m_config.spike_multiplier * jitter
            });
            const double outlier_threshold = median_rtt + outlier_tolerance;
            if (effective_rtt > outlier_threshold) {
                // The spike stays out of the EMA but still enters the median window,
                // so a sustained rise is eventually learned instead of rejected forever.
                effective_rtt = median_rtt;
                res.spike_filtered = true;
            }
        } else {
            const double cold_start_cap = (samples_before == 0)
                ? 200.0
                : (baseline_rtt + std::max(constants::MIN_OUTLIER_TOLERANCE_MS, baseline_rtt * 0.5));
            if (effective_rtt > cold_start_cap) {
                effective_rtt = cold_start_cap;
                window_sample = cold_start_cap;
                res.cold_start_guard = true;
            }
        }

        m_rtt_tracker.add_sample(window_sample, effective_rtt);
    }

    res.measured_rtt_ms = measured_rtt;
    res.smoothed_rtt_ms = m_rtt_tracker.get_smoothed_rtt_ms();

    // Above the ceiling is either a malformed packet or a legitimately long lock such
    // as a Limit Break's. Neither is safe to shorten, so it passes through untouched.
    if (original_lock_ms > m_config.max_animation_lock_ms) {
        res.adjusted_lock_ms = original_lock_ms;
        res.clamped_by_ceiling = true;
        return res;
    }

    // Already at or under the floor: there is nothing to take off.
    if (original_lock_ms <= m_config.min_animation_lock_ms) {
        res.adjusted_lock_ms = original_lock_ms;
        return res;
    }

    double latency_delta = (effective_rtt - m_config.target_ping_ms) - m_config.safety_margin_ms;
    if (latency_delta < 0.0) {
        latency_delta = 0.0;
    }

    double target_lock = original_lock_ms - latency_delta;

    if (target_lock < m_config.min_animation_lock_ms) {
        target_lock = m_config.min_animation_lock_ms;
        res.clamped_by_floor = true;
        ++m_total_floor_clamps;
    }

    res.adjusted_lock_ms = target_lock;
    res.delay_reduced_ms = std::max(0.0, original_lock_ms - res.adjusted_lock_ms);

    if (m_config.dry_run || !m_config.enabled) {
        res.applied = false;
    } else {
        res.applied = (res.delay_reduced_ms > 0.0);
    }

    if (res.applied) {
        ++m_total_actions_mitigated;
        m_cumulative_time_saved_ms += res.delay_reduced_ms;
    }

    return res;
}

void AnimationLockMitigator::record_cast_begin(ActionId action_id, float cast_time_seconds, TimePoint now) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cast_tracker.on_cast_begin(action_id, cast_time_seconds, now);
}

void AnimationLockMitigator::record_cast_interrupt(TimePoint now) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cast_tracker.on_cast_interrupt(now);
}

void AnimationLockMitigator::record_cast_end(TimePoint now) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cast_tracker.on_cast_end(now);
}

bool AnimationLockMitigator::is_casting(TimePoint now) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_cast_tracker.is_casting(now, m_rtt_tracker.get_smoothed_rtt_ms());
}

MitigationConfig AnimationLockMitigator::get_config() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_config;
}

void AnimationLockMitigator::set_config(const MitigationConfig& config) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config = sanitize(config);
    m_rtt_tracker.set_window_size(m_config.rtt_sample_window);
}

void AnimationLockMitigator::set_dry_run(bool dry_run) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config.dry_run = dry_run;
}

void AnimationLockMitigator::set_enabled(bool enabled) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config.enabled = enabled;
}

void AnimationLockMitigator::set_target_ping_ms(double target_ping_ms) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config.target_ping_ms = target_ping_ms;
    m_config = sanitize(m_config);
}

void AnimationLockMitigator::set_min_animation_lock_ms(double min_lock_ms) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config.min_animation_lock_ms = min_lock_ms;
    m_config = sanitize(m_config);
}

void AnimationLockMitigator::set_spike_multiplier(double multiplier) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config.spike_multiplier = multiplier;
    m_config = sanitize(m_config);
}

SessionStats AnimationLockMitigator::get_session_stats() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return SessionStats{
        .total_actions_requested = m_total_actions_requested,
        .total_actions_mitigated = m_total_actions_mitigated,
        .cumulative_time_saved_ms = m_cumulative_time_saved_ms,
        .current_smoothed_rtt_ms = m_rtt_tracker.get_smoothed_rtt_ms(),
        .current_jitter_ms = m_rtt_tracker.get_jitter_ms(),
        .total_floor_clamps = m_total_floor_clamps
    };
}

void AnimationLockMitigator::reset() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_seq_tracker.clear();
    m_cast_tracker.reset();
    m_rtt_tracker.reset(m_config.target_ping_ms * 3.0);
    m_total_actions_requested = 0;
    m_total_actions_mitigated = 0;
    m_cumulative_time_saved_ms = 0.0;
    m_total_floor_clamps = 0;
}

} // namespace hub::mitigator
