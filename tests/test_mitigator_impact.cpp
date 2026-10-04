#include "test_framework.hpp"
#include "app/mitigator_impact.hpp"

#include <vector>

using namespace hub;
using namespace hub::app;

namespace {

constexpr float kCeiling = 2500.0f;

ipc::MitigatorTelemetryPayload sample(float rtt, float raw, float adjusted) {
    ipc::MitigatorTelemetryPayload s{};
    s.measured_rtt_ms = rtt;
    s.original_lock_ms = raw;
    s.adjusted_lock_ms = adjusted;
    s.delay_reduced_ms = raw - adjusted;
    s.applied = adjusted < raw ? 1 : 0;
    return s;
}

} // namespace

TEST_CASE(MitigatorImpact, MitigatedActionWaitsRoundTripPlusWrittenLock) {
    const auto t = weave_timing(sample(213.0f, 600.0f, 402.0f), kCeiling);
    TEST_ASSERT_TRUE(t.has_value());
    TEST_ASSERT_NEAR(t->without_ms, 813.0f, 0.01);
    TEST_ASSERT_NEAR(t->with_ms, 615.0f, 0.01);
    TEST_ASSERT_NEAR(t->saved_ms(), 198.0f, 0.01);
}

TEST_CASE(MitigatorImpact, SwitchedOffActionWaitsTheSameEitherWay) {
    const auto t = weave_timing(sample(213.0f, 600.0f, 600.0f), kCeiling);
    TEST_ASSERT_TRUE(t.has_value());
    TEST_ASSERT_NEAR(t->with_ms, t->without_ms, 0.01);
}

TEST_CASE(MitigatorImpact, DryRunReportsTheLockItWouldWrite) {
    auto s = sample(213.0f, 600.0f, 402.0f);
    s.dry_run = 1;
    s.applied = 0;
    const auto t = weave_timing(s, kCeiling);
    TEST_ASSERT_TRUE(t.has_value());
    TEST_ASSERT_NEAR(t->saved_ms(), 198.0f, 0.01);
}

TEST_CASE(MitigatorImpact, CastsUnmatchedRepliesAndLongLocksAreLeftOut) {
    auto cast = sample(213.0f, 100.0f, 100.0f);
    cast.cast_active = 1;
    TEST_ASSERT_FALSE(weave_timing(cast, kCeiling).has_value());
    TEST_ASSERT_FALSE(weave_timing(sample(0.0f, 3100.0f, 3100.0f), kCeiling).has_value());
    TEST_ASSERT_FALSE(weave_timing(sample(213.0f, 3100.0f, 3100.0f), kCeiling).has_value());
}

TEST_CASE(MitigatorImpact, SummaryAveragesOnlyComparableActions) {
    auto cast = sample(0.0f, 100.0f, 100.0f);
    cast.cast_active = 1;
    const std::vector<ipc::MitigatorTelemetryPayload> recent = {
        sample(200.0f, 600.0f, 415.0f), cast, sample(220.0f, 600.0f, 395.0f),
    };
    const ImpactSummary s = summarize_impact(recent, kCeiling);
    TEST_ASSERT_EQ(s.samples, 2u);
    TEST_ASSERT_NEAR(s.avg_rtt_ms, 210.0f, 0.01);
    TEST_ASSERT_NEAR(s.avg_server_lock_ms, 600.0f, 0.01);
    TEST_ASSERT_NEAR(s.avg_applied_lock_ms, 405.0f, 0.01);
    TEST_ASSERT_NEAR(s.avg_without_ms, 810.0f, 0.01);
    TEST_ASSERT_NEAR(s.avg_with_ms, 615.0f, 0.01);
    TEST_ASSERT_NEAR(s.avg_saved_ms(), 195.0f, 0.01);
}

TEST_CASE(MitigatorImpact, SummaryKeepsOnlyTheNewestWindow) {
    std::vector<ipc::MitigatorTelemetryPayload> recent;
    for (int i = 0; i < 5; ++i) recent.push_back(sample(500.0f, 600.0f, 600.0f)); // old, unmitigated
    for (int i = 0; i < 3; ++i) recent.push_back(sample(200.0f, 600.0f, 415.0f)); // newest
    const ImpactSummary s = summarize_impact(recent, kCeiling, 3);
    TEST_ASSERT_EQ(s.samples, 3u);
    TEST_ASSERT_NEAR(s.avg_rtt_ms, 200.0f, 0.01);
    TEST_ASSERT_NEAR(s.avg_saved_ms(), 185.0f, 0.01);
}

TEST_CASE(MitigatorImpact, EmptyTelemetryGivesNoSamples) {
    const ImpactSummary s = summarize_impact({}, kCeiling);
    TEST_ASSERT_EQ(s.samples, 0u);
    TEST_ASSERT_NEAR(s.avg_saved_ms(), 0.0f, 0.01);
}
