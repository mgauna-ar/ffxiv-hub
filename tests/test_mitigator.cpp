#include "test_framework.hpp"
#include "common/config/json.hpp"
#include "mitigator/rolling_rtt.hpp"
#include "mitigator/sequence_tracker.hpp"
#include "mitigator/cast_tracker.hpp"
#include "mitigator/animation_lock.hpp"
#include "mitigator/latency_plugin.hpp"
#include <thread>
#include <chrono>

using namespace hub::mitigator;

TEST_CASE(Mitigator, RttTrackerBasicEmaAndJitter) {
    RollingRttTracker tracker(10, 50.0);
    TEST_ASSERT_EQ(tracker.sample_count(), 0u);

    tracker.add_sample(100.0);
    TEST_ASSERT_EQ(tracker.sample_count(), 1u);
    TEST_ASSERT_NEAR(tracker.get_smoothed_rtt_ms(), 100.0, 0.01);
    TEST_ASSERT_NEAR(tracker.get_jitter_ms(), 0.0, 0.01);

    tracker.add_sample(80.0);
    TEST_ASSERT_EQ(tracker.sample_count(), 2u);
    // Alpha for n=2 is 2/(2+1) = 2/3
    // EMA = (2/3)*80 + (1/3)*100 = 53.33 + 33.33 = 86.66
    TEST_ASSERT_NEAR(tracker.get_smoothed_rtt_ms(), 86.66, 0.1);
    TEST_ASSERT_TRUE(tracker.get_jitter_ms() > 0.0);
}

TEST_CASE(Mitigator, RttTrackerMedian) {
    RollingRttTracker tracker(5);
    tracker.add_sample(10.0);
    tracker.add_sample(50.0);
    tracker.add_sample(30.0);
    tracker.add_sample(20.0);
    tracker.add_sample(40.0);

    // Sorted: 10, 20, 30, 40, 50 -> Median is 30.0
    TEST_ASSERT_NEAR(tracker.get_median_rtt_ms(), 30.0, 0.01);

    tracker.add_sample(60.0);
    // Window of 5: 50, 30, 20, 40, 60 -> Sorted: 20, 30, 40, 50, 60 -> Median is 40.0
    TEST_ASSERT_NEAR(tracker.get_median_rtt_ms(), 40.0, 0.01);
}

TEST_CASE(Mitigator, RttTrackerPlausibleFiltering) {
    RollingRttTracker tracker(5, 50.0);
    tracker.add_sample(-10.0); // Negative
    tracker.add_sample(0.1);   // Too small (< 0.5)
    tracker.add_sample(9999.0); // Extreme (> 5000)

    TEST_ASSERT_EQ(tracker.sample_count(), 0u);
    TEST_ASSERT_NEAR(tracker.get_smoothed_rtt_ms(), 50.0, 0.01);
}

TEST_CASE(Mitigator, SequenceTrackerExactAndFifoMatching) {
    SequenceTracker tracker(std::chrono::milliseconds(5000));
    const auto t0 = std::chrono::steady_clock::now();

    // Strategy 1: Exact non-zero sequence match
    tracker.record_request(1001, 42, t0);
    tracker.record_request(1002, 43, t0);

    auto m1 = tracker.match_response(1002, 43, t0 + std::chrono::milliseconds(50));
    TEST_ASSERT(m1.has_value());
    TEST_ASSERT_EQ(m1->action_id, 1002u);
    TEST_ASSERT_EQ(m1->sequence, 43u);

    // Strategy 2: FIFO match by action_id when server sequence differs (queued action N vs N+1)
    tracker.record_request(2001, 10, t0);
    auto m2 = tracker.match_response(2001, 11, t0 + std::chrono::milliseconds(60));
    TEST_ASSERT(m2.has_value());
    TEST_ASSERT_EQ(m2->action_id, 2001u);
    TEST_ASSERT_EQ(m2->sequence, 10u);

    // Unmatched action
    auto m3 = tracker.match_response(9999, 9999, t0);
    TEST_ASSERT_FALSE(m3.has_value());
}

TEST_CASE(Mitigator, CastTrackerActiveStateAndGrace) {
    CastTracker tracker;
    const auto t0 = std::chrono::steady_clock::now();

    TEST_ASSERT_FALSE(tracker.is_casting(t0));

    // Begin 2.5s cast
    tracker.on_cast_begin(3001, 2.5f, t0);
    TEST_ASSERT_TRUE(tracker.is_casting(t0 + std::chrono::milliseconds(1000)));
    TEST_ASSERT_EQ(tracker.current_cast_action_id(), 3001u);
    TEST_ASSERT_NEAR(tracker.remaining_cast_time_seconds(t0 + std::chrono::milliseconds(1000)), 1.5f, 0.05f);

    // After 2.5s + 0.05s grace window (2.55s): still in grace
    TEST_ASSERT_TRUE(tracker.is_casting(t0 + std::chrono::milliseconds(2550)));

    // After 3.5s: finished
    TEST_ASSERT_FALSE(tracker.is_casting(t0 + std::chrono::milliseconds(3500)));

    // Interrupted cast
    tracker.on_cast_begin(3002, 3.0f, t0);
    tracker.on_cast_interrupt(t0 + std::chrono::milliseconds(500));
    TEST_ASSERT_FALSE(tracker.is_casting(t0 + std::chrono::milliseconds(600)));
}

TEST_CASE(Mitigator, AnimationLockStandardMitigation) {
    MitigationConfig cfg;
    cfg.target_ping_ms = 15.0;
    cfg.min_animation_lock_ms = 25.0;
    cfg.dry_run = false;

    AnimationLockMitigator mit(cfg);
    const auto t0 = std::chrono::steady_clock::now();

    // Simulate action with 100ms RTT and 600ms original server lock
    mit.record_action_request(100, 1, t0);
    const auto res = mit.calculate_mitigation(100, 1, 600.0, t0 + std::chrono::milliseconds(100));

    TEST_ASSERT_TRUE(res.applied);
    TEST_ASSERT_FALSE(res.cast_active);
    TEST_ASSERT_NEAR(res.measured_rtt_ms, 100.0, 1.0);
    // Delta = 100 - 15 = 85ms
    // Adjusted = 600 - 85 = 515ms
    TEST_ASSERT_NEAR(res.adjusted_lock_ms, 515.0, 1.0);
    TEST_ASSERT_NEAR(res.delay_reduced_ms, 85.0, 1.0);
}

TEST_CASE(Mitigator, AnimationLockFloorClamping) {
    MitigationConfig cfg;
    cfg.target_ping_ms = 15.0;
    cfg.min_animation_lock_ms = 25.0;

    AnimationLockMitigator mit(cfg);
    const auto t0 = std::chrono::steady_clock::now();

    // Action with short original lock (e.g. 50ms) and high RTT (100ms)
    mit.record_action_request(101, 2, t0);
    const auto res = mit.calculate_mitigation(101, 2, 50.0, t0 + std::chrono::milliseconds(100));

    // Target = 50 - 85 = -35ms -> clamped to floor (25ms)
    TEST_ASSERT_TRUE(res.clamped_by_floor);
    TEST_ASSERT_NEAR(res.adjusted_lock_ms, 25.0, 0.01);
    TEST_ASSERT_NEAR(res.delay_reduced_ms, 25.0, 0.01);
}

TEST_CASE(Mitigator, AnimationLockCasterTaxPreservation) {
    MitigationConfig cfg;
    AnimationLockMitigator mit(cfg);
    const auto t0 = std::chrono::steady_clock::now();

    // Begin casted spell
    mit.record_cast_begin(200, 2.5f, t0);
    mit.record_action_request(200, 3, t0, true, 2.5f);

    // Effect arrives at 2.6s (after cast completed)
    const auto res = mit.calculate_mitigation(200, 3, 100.0, t0 + std::chrono::milliseconds(2600));

    // Invariant: Cast actions must preserve the native 100ms lock (unmitigated)
    TEST_ASSERT_TRUE(res.cast_active);
    TEST_ASSERT_FALSE(res.applied);
    TEST_ASSERT_NEAR(res.adjusted_lock_ms, 100.0, 0.01);
    TEST_ASSERT_NEAR(res.delay_reduced_ms, 0.0, 0.01);
}

TEST_CASE(Mitigator, AnimationLockDryRunMode) {
    MitigationConfig cfg;
    cfg.dry_run = true;
    AnimationLockMitigator mit(cfg);
    const auto t0 = std::chrono::steady_clock::now();

    mit.record_action_request(300, 4, t0);
    const auto res = mit.calculate_mitigation(300, 4, 600.0, t0 + std::chrono::milliseconds(100));

    TEST_ASSERT_FALSE(res.applied);
    TEST_ASSERT_TRUE(res.delay_reduced_ms > 0.0);
    TEST_ASSERT_EQ(mit.get_session_stats().total_actions_mitigated, 0u);
}

TEST_CASE(Mitigator, LatencyPluginConfigSerialization) {
    LatencyPlugin plugin;
    hub::config::JsonValue doc{hub::config::JsonValue::ObjectType{}};

    plugin.serialize_config(doc);
    TEST_ASSERT_TRUE(doc.contains("target_ping_ms"));
    TEST_ASSERT_NEAR(doc["target_ping_ms"].as_double(), 15.0, 0.01);

    doc["target_ping_ms"] = hub::config::JsonValue(20.0);
    doc["min_animation_lock_ms"] = hub::config::JsonValue(30.0);
    plugin.deserialize_config(doc);

    TEST_ASSERT_NEAR(plugin.mitigator().get_config().target_ping_ms, 20.0, 0.01);
    TEST_ASSERT_NEAR(plugin.mitigator().get_config().min_animation_lock_ms, 30.0, 0.01);
}
