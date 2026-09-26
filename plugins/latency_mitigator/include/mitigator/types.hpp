#pragma once

#include "common/ui/overlay_config.hpp"
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

    /// Ping grading bands, shared by the in-game HUD dot and the desktop latency view.
    constexpr double PING_GRADE_GOOD_MS = 180.0;
    constexpr double PING_GRADE_FAIR_MS = 260.0;
    constexpr double PING_GRADE_POOR_MS = 340.0;

    constexpr size_t MIN_SAMPLES_FOR_MEDIAN_FILTER = 5;
    constexpr size_t MAX_RTT_SAMPLE_WINDOW = 64;
    constexpr double MIN_OUTLIER_TOLERANCE_MS = 50.0;
    /// Share of the reference RTT (the median, or during cold start the EMA baseline)
    /// a sample may exceed it by before it counts as a spike.
    constexpr double RELATIVE_OUTLIER_TOLERANCE = 0.5;
    /// Cap on the very first RTT sample, which has no baseline to be judged against.
    constexpr double COLD_START_FIRST_SAMPLE_CAP_MS = 200.0;
    constexpr double JITTER_SPIKE_MULTIPLIER = 2.5;

    /// Default HUD placement and appearance. Also the target of a geometry reset.
    constexpr float DEFAULT_OVERLAY_X = 20.0f;
    constexpr float DEFAULT_OVERLAY_Y = 20.0f;
    constexpr float DEFAULT_OVERLAY_WIDTH = 120.0f;
    constexpr float DEFAULT_OVERLAY_HEIGHT = 32.0f;
    constexpr float DEFAULT_OVERLAY_OPACITY = 0.90f;
    constexpr float MIN_OVERLAY_OPACITY = 0.10f;
    constexpr float MAX_OVERLAY_OPACITY = 1.0f;
    constexpr float DEFAULT_OVERLAY_SCALE = 1.0f;
    constexpr float MIN_OVERLAY_SCALE = 0.50f;
    constexpr float MAX_OVERLAY_SCALE = 3.0f;
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
    bool   enabled{true};
};

/// Starting point for the HUD's shared overlay settings, and the fallback when
/// no overlay instance exists to read live state from.
[[nodiscard]] constexpr ui::OverlayConfig default_overlay_config() noexcept {
    return ui::OverlayConfig{
        .opacity = constants::DEFAULT_OVERLAY_OPACITY,
        .scale = constants::DEFAULT_OVERLAY_SCALE,
        .x = constants::DEFAULT_OVERLAY_X,
        .y = constants::DEFAULT_OVERLAY_Y,
        .width = constants::DEFAULT_OVERLAY_WIDTH,
        .height = constants::DEFAULT_OVERLAY_HEIGHT,
    };
}

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
