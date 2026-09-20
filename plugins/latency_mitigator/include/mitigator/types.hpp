#pragma once

#include <cstdint>
#include <chrono>
#include <string>

namespace hub::mitigator {

using ActionId = uint32_t;
using SequenceId = uint32_t;
using TimePoint = std::chrono::steady_clock::time_point;
using Milliseconds = std::chrono::duration<double, std::milli>;

namespace constants {
    constexpr double MS_PER_SECOND = 1000.0;
    constexpr double SECONDS_PER_MS = 0.001;

    constexpr double DEFAULT_TARGET_PING_MS = 15.0;
    constexpr double DEFAULT_MIN_ANIMATION_LOCK_MS = 25.0;
    constexpr double DEFAULT_MAX_ANIMATION_LOCK_MS = 2500.0;
    constexpr size_t DEFAULT_RTT_SAMPLE_WINDOW = 10;
    constexpr double DEFAULT_INITIAL_RTT_MS = 50.0;
    constexpr double DEFAULT_SAFETY_MARGIN_MS = 0.0;

    constexpr double MIN_PLAUSIBLE_RTT_MS = 0.5;
    constexpr double MAX_PLAUSIBLE_RTT_MS = 5000.0;

    constexpr auto DEFAULT_STALE_TIMEOUT = std::chrono::milliseconds(5000);
    constexpr auto GENERIC_FALLBACK_MAX_ELAPSED = std::chrono::milliseconds(1500);

    constexpr double ABSOLUTE_MIN_ANIMATION_LOCK_FLOOR_MS = 20.0;

    constexpr float CAST_COMPLETION_GRACE_WINDOW_SECONDS = 0.1f;
    constexpr float CAST_GRACE_BASE_BUFFER_SECONDS = 0.050f;
    constexpr float ABSOLUTE_MAX_CAST_DURATION_SECONDS = 30.0f;
    constexpr double ONE_WAY_LATENCY_RATIO = 0.5;

    constexpr size_t MIN_SAMPLES_FOR_MEDIAN_FILTER = 5;
    constexpr double MIN_OUTLIER_TOLERANCE_MS = 50.0;
    constexpr double JITTER_SPIKE_MULTIPLIER = 3.0;
}

/// Configuration parameters for latency mitigation
struct MitigationConfig {
    double target_ping_ms{constants::DEFAULT_TARGET_PING_MS};
    double min_animation_lock_ms{constants::DEFAULT_MIN_ANIMATION_LOCK_MS};
    double max_animation_lock_ms{constants::DEFAULT_MAX_ANIMATION_LOCK_MS};
    size_t rtt_sample_window{constants::DEFAULT_RTT_SAMPLE_WINDOW};
    double safety_margin_ms{constants::DEFAULT_SAFETY_MARGIN_MS};
    double spike_multiplier{constants::JITTER_SPIKE_MULTIPLIER};
    bool   dry_run{false};
    bool   verbose{false};
    bool   enabled{true};
    bool   auto_start{false};
    bool   notifications_enabled{true};
};

/// In-game micro ping HUD layout mode
enum class OverlayDisplayMode : uint32_t {
    CompactInline,
    TwoRow,
    PingOnly,
};

/// Record of an action request sent from client to server
struct ActionRequestInfo {
    ActionId   action_id{0};
    SequenceId sequence{0};
    TimePoint  timestamp{std::chrono::steady_clock::now()};
    bool       is_cast{false};
    float      cast_duration_seconds{0.0f};
};

/// Result of animation lock calculation for a server response
struct MitigationResult {
    ActionId   action_id{0};
    SequenceId sequence{0};

    double original_lock_ms{0.0};
    double adjusted_lock_ms{0.0};
    double delay_reduced_ms{0.0};
    double measured_rtt_ms{0.0};
    double smoothed_rtt_ms{0.0};

    bool clamped_by_floor{false};
    bool clamped_by_ceiling{false};
    bool applied{false};
    bool cast_active{false};
    bool spike_filtered{false};
    bool cold_start_guard{false};
};

/// Real-time session telemetry summary
struct SessionStats {
    uint64_t total_actions_requested{0};
    uint64_t total_actions_mitigated{0};
    double   cumulative_time_saved_ms{0.0};
    double   current_smoothed_rtt_ms{0.0};
    double   current_jitter_ms{0.0};
    uint64_t total_floor_clamps{0};
};

} // namespace hub::mitigator
