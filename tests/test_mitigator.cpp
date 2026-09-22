#include "test_framework.hpp"
#include "common/config/json.hpp"
#include "common/ipc/protocol.hpp"
#include "hub/game_definitions.hpp"
#include "mitigator/rolling_rtt.hpp"
#include "mitigator/sequence_tracker.hpp"
#include "mitigator/cast_tracker.hpp"
#include "mitigator/animation_lock.hpp"
#include "mitigator/latency_plugin.hpp"
#include <thread>
#include <chrono>
#include <cstring>

using namespace hub;
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

    // spike_multiplier round-trips too (added alongside the desktop UI's live slider)
    TEST_ASSERT_NEAR(plugin.mitigator().get_config().spike_multiplier, 3.0, 0.01);
    doc["spike_multiplier"] = hub::config::JsonValue(2.5);
    plugin.deserialize_config(doc);
    TEST_ASSERT_NEAR(plugin.mitigator().get_config().spike_multiplier, 2.5, 0.01);
}

TEST_CASE(Mitigator, SpikeMultiplierAffectsOutlierThreshold) {
    // A tighter multiplier should flag an RTT spike that a looser one tolerates,
    // once the multiplier*jitter term dominates the outlier tolerance formula
    // (median*0.5 and the 50ms floor are the same for both, by construction).
    AnimationLockMitigator mit_tight;
    mit_tight.set_spike_multiplier(1.0);
    AnimationLockMitigator mit_loose;
    mit_loose.set_spike_multiplier(10.0);

    auto t0 = std::chrono::steady_clock::now();
    // Seed 5 noisy-but-plausible samples (alternating 20ms/80ms) so the
    // median-filter path is active and jitter is well above zero.
    const int seed_elapsed_ms[5] = {20, 80, 20, 80, 20};
    for (int i = 0; i < 5; ++i) {
        const auto req_time = t0 + std::chrono::milliseconds(i * 200);
        mit_tight.record_action_request(100 + i, i, req_time);
        (void)mit_tight.calculate_mitigation(100 + i, i, 600.0, req_time + std::chrono::milliseconds(seed_elapsed_ms[i]));
        mit_loose.record_action_request(100 + i, i, req_time);
        (void)mit_loose.calculate_mitigation(100 + i, i, 600.0, req_time + std::chrono::milliseconds(seed_elapsed_ms[i]));
    }

    TEST_ASSERT_TRUE(mit_tight.get_rtt_tracker().get_jitter_ms() > 5.0);

    // A spike far enough above the median to clear the tight tolerance
    // (median + max(50, median*0.5, 1*jitter)) but stay inside the loose one
    // (median + max(50, median*0.5, 10*jitter)).
    const auto spike_time = t0 + std::chrono::milliseconds(5 * 200);
    mit_tight.record_action_request(200, 5, spike_time);
    const auto res_tight = mit_tight.calculate_mitigation(200, 5, 600.0, spike_time + std::chrono::milliseconds(150));

    mit_loose.record_action_request(200, 5, spike_time);
    const auto res_loose = mit_loose.calculate_mitigation(200, 5, 600.0, spike_time + std::chrono::milliseconds(150));

    TEST_ASSERT_TRUE(res_tight.spike_filtered);
    TEST_ASSERT_FALSE(res_loose.spike_filtered);
}

TEST_CASE(Mitigator, LatencyPluginGatesOnLocalPlayerLockChange) {
    // ReceiveActionEffect fires for every actor in the zone; on_receive_action_effect
    // must only mitigate when THIS call actually changed our own ActionManager's
    // animation_lock, not merely because the packet header carries a value.
    LatencyPlugin plugin;
    plugin.initialize();

    ipc::PacketRingBuffer ring;
    plugin.set_ring_buffer(&ring);
    plugin.set_connected(true);

    std::vector<uint8_t> mgr_buf(0x200, 0);
    void* mgr = mgr_buf.data();

    // Queue a request, but the "original engine call" never changes the lock
    // (e.g. this ReceiveActionEffect belongs to another actor).
    plugin.on_use_action_location(mgr, 0, 700, 0, nullptr, 0, /*result=*/1);
    plugin.on_pre_receive_action_effect(); // snapshots old_lock = 0.0

    game::ActionEffectHeader hdr{};
    hdr.action_id = 700;
    hdr.source_sequence = 0;
    plugin.on_receive_action_effect(0, nullptr, &hdr, nullptr, nullptr);

    std::vector<uint8_t> item;
    TEST_ASSERT_FALSE(ring.pop(item));
}

TEST_CASE(Mitigator, ResolvedActionManagerObservesLockWithoutPriorAction) {
    // The sigscanned instance is what lets the very first action effect be seen;
    // without it there is no ActionManager until UseActionLocation has fired.
    LatencyPlugin plugin;
    plugin.initialize();

    ipc::PacketRingBuffer ring;
    plugin.set_ring_buffer(&ring);
    plugin.set_connected(true);

    std::vector<uint8_t> mgr_buf(0x200, 0);
    plugin.on_action_manager_resolved(mgr_buf.data());

    plugin.on_pre_receive_action_effect();

    const float server_lock_seconds = 0.6f;
    std::memcpy(mgr_buf.data() + game::offsets::ACTION_MANAGER_ANIMATION_LOCK, &server_lock_seconds, sizeof(float));

    game::ActionEffectHeader hdr{};
    hdr.action_id = 900;
    hdr.source_sequence = 0;
    plugin.on_receive_action_effect(0, nullptr, &hdr, nullptr, nullptr);

    std::vector<uint8_t> item;
    TEST_ASSERT_TRUE(ring.pop(item));
}

TEST_CASE(Mitigator, ResolvedActionManagerIgnoresNull) {
    // A failed scan must not clear a pointer UseActionLocation already supplied.
    LatencyPlugin plugin;
    plugin.initialize();

    ipc::PacketRingBuffer ring;
    plugin.set_ring_buffer(&ring);
    plugin.set_connected(true);

    std::vector<uint8_t> mgr_buf(0x200, 0);
    plugin.on_use_action_location(mgr_buf.data(), 0, 500, 0, nullptr, 0, /*result=*/1);
    plugin.on_action_manager_resolved(nullptr);

    plugin.on_pre_receive_action_effect();

    const float server_lock_seconds = 0.5f;
    std::memcpy(mgr_buf.data() + game::offsets::ACTION_MANAGER_ANIMATION_LOCK, &server_lock_seconds, sizeof(float));

    game::ActionEffectHeader hdr{};
    hdr.action_id = 500;
    hdr.source_sequence = 0;
    plugin.on_receive_action_effect(0, nullptr, &hdr, nullptr, nullptr);

    std::vector<uint8_t> item;
    TEST_ASSERT_TRUE(ring.pop(item));
}

TEST_CASE(Mitigator, LatencyPluginEmitsTelemetryAndAppliesWriteBack) {
    LatencyPlugin plugin;
    plugin.initialize();

    ipc::PacketRingBuffer ring;
    plugin.set_ring_buffer(&ring);
    plugin.set_connected(true);

    std::vector<uint8_t> mgr_buf(0x200, 0);
    void* mgr = mgr_buf.data();

    plugin.on_use_action_location(mgr, 0, 500, 0, nullptr, 0, /*result=*/1);
    plugin.on_pre_receive_action_effect(); // snapshots old_lock = 0.0

    // Sleep past the default target_ping_ms (15ms) so the measured RTT produces
    // a nonzero mitigation, exercising the write-back path.
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // Simulate the original engine function writing the server's lock value.
    const float server_lock_seconds = 0.6f;
    std::memcpy(mgr_buf.data() + game::offsets::ACTION_MANAGER_ANIMATION_LOCK, &server_lock_seconds, sizeof(float));

    game::ActionEffectHeader hdr{};
    hdr.action_id = 500;
    hdr.source_sequence = 0;
    plugin.on_receive_action_effect(0, nullptr, &hdr, nullptr, nullptr);

    std::vector<uint8_t> item;
    TEST_ASSERT_TRUE(ring.pop(item));

    auto header = ipc::deserialize_header(item);
    TEST_ASSERT_TRUE(header.has_value());
    TEST_ASSERT(header->plugin_id == static_cast<uint16_t>(PluginId::LatencyMitigator));
    TEST_ASSERT(header->message_type == static_cast<uint16_t>(MessageType::MitigatorTelemetry));
    TEST_ASSERT_EQ(header->payload_size, static_cast<uint32_t>(sizeof(ipc::MitigatorTelemetryPayload)));

    ipc::MitigatorTelemetryPayload payload{};
    std::memcpy(&payload, item.data() + sizeof(ipc::PacketHeader), sizeof(payload));
    TEST_ASSERT_EQ(payload.action_id, 500u);
    TEST_ASSERT_TRUE(payload.delay_reduced_ms > 0.0f);
    TEST_ASSERT(payload.applied == 1);

    float post_lock = 0.0f;
    std::memcpy(&post_lock, mgr_buf.data() + game::offsets::ACTION_MANAGER_ANIMATION_LOCK, sizeof(float));
    TEST_ASSERT_TRUE(post_lock < server_lock_seconds);
}

TEST_CASE(Mitigator, DisabledPluginIgnoresHooksAndPersistsTheChoice) {
    // The plugin-level switch is coarser than the mitigation switch: it has to
    // stop the plugin from seeing actions at all, not just from writing back.
    LatencyPlugin plugin;
    plugin.initialize();

    ipc::PacketRingBuffer ring;
    plugin.set_ring_buffer(&ring);
    plugin.set_connected(true);
    plugin.set_plugin_enabled(false);

    std::vector<uint8_t> mgr_buf(0x200, 0);
    void* mgr = mgr_buf.data();

    plugin.on_use_action_location(mgr, 0, 700, 0, nullptr, 0, /*result=*/1);
    TEST_ASSERT_EQ(plugin.mitigator().get_sequence_tracker().pending_count(), 0u);

    plugin.on_pre_receive_action_effect();
    game::ActionEffectHeader hdr{};
    hdr.action_id = 700;
    plugin.on_receive_action_effect(0, nullptr, &hdr, nullptr, nullptr);

    std::vector<uint8_t> item;
    TEST_ASSERT_FALSE(ring.pop(item));

    // Mitigation keeps its own preference: the coarse switch must not rewrite it.
    TEST_ASSERT_TRUE(plugin.mitigator().get_config().enabled);

    hub::config::JsonValue json{hub::config::JsonValue::ObjectType{}};
    plugin.serialize_config(json);
    TEST_ASSERT_FALSE(json["plugin_enabled"].as_bool(true));
    TEST_ASSERT_TRUE(json["enabled"].as_bool(false));

    plugin.set_plugin_enabled(true);
    plugin.deserialize_config(json);
    TEST_ASSERT_FALSE(plugin.is_plugin_enabled());

    plugin.set_plugin_enabled(true);
    plugin.on_use_action_location(mgr, 0, 700, 0, nullptr, 0, /*result=*/1);
    TEST_ASSERT_EQ(plugin.mitigator().get_sequence_tracker().pending_count(), 1u);
}
