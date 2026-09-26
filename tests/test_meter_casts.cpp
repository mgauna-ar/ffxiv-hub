#include "test_framework.hpp"
#include "hub/game/actions.hpp"
#include "meter/action_decoder.hpp"
#include "meter/combat_plugin.hpp"
#include "meter/encounter_engine.hpp"
#include "meter/gcd_uptime.hpp"
#include "meter/metrics_accumulator.hpp"
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

using namespace hub::meter;

namespace {

constexpr EntityId kSam = 0x10000001;
constexpr EntityId kSch = 0x10000002;
constexpr EntityId kEos = 0x40000100;
constexpr EntityId kBoss = 0x40000001;

// Action ids, pinned by name in GcdAndPressTablesComeFromTheSheet.
constexpr ActionId kHeavySwing = 31;      // Instant GCD
constexpr ActionId kBerserk = 38;         // oGCD
constexpr ActionId kGlareIII = 25859;     // 1.5 s cast
constexpr ActionId kJoltIII = 37004;      // 2.0 s cast
constexpr ActionId kVerthunderIII = 25855; // 5.0 s cast, Dualcast makes it instant
constexpr ActionId kFireInRed = 34650;    // 1.5 s cast
constexpr ActionId kCreatureMotif = 34689; // 3.0 s cast on a 4.0 s GCD
constexpr ActionId kSpinningEdge = 2240;
constexpr ActionId kTen = 2259;           // The GCD is its additional cooldown group
constexpr ActionId kChi = 18806;          // 0.5 s GCD
constexpr ActionId kRaiton = 2267;        // 1.5 s GCD
constexpr ActionId kDrill = 16498;
constexpr ActionId kKardiaHeal = 28119;   // Fired by the game on each of the Sage's hits
constexpr ActionId kEudaimonia = 37036;
constexpr ActionId kBraver = 200;         // Limit Break

/// A pull a long way into the steady clock: a packet stamped 0 means "unstamped".
constexpr uint64_t kT0 = 1'000'000'000;

uint64_t at(double s) {
    return kT0 + static_cast<uint64_t>(std::llround(s * 1e6));
}

struct Timeline {
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    [[nodiscard]] std::chrono::steady_clock::time_point at(double s) const {
        return t0 + std::chrono::milliseconds(static_cast<int64_t>(s * 1000.0));
    }
};

hub::ipc::CastPacket cast(EntityId source, ActionId action, double s) {
    hub::ipc::CastPacket packet{};
    packet.source_id = source;
    packet.action_id = action;
    packet.timestamp_us = at(s);
    return packet;
}

hub::ipc::CombatActionPacket hit(EntityId source, double s) {
    hub::ipc::CombatActionPacket packet{};
    packet.source_id = source;
    packet.target_id = kBoss;
    packet.action_id = kHeavySwing;
    packet.damage = 1000;
    packet.effect_type = static_cast<uint16_t>(EffectType::Damage);
    packet.timestamp_us = at(s);
    return packet;
}

void register_party(CombatantRegistry& registry) {
    registry.register_actor(kSam, "Sam", Job::SAM, 0, ActorType::Player);
    registry.register_actor(kSch, "Sch", Job::SCH, 0, ActorType::Player);
    registry.register_actor(kBoss, "Boss", Job::None, 0, ActorType::Monster);
}

const CombatantStats* row_of(const EncounterSummary& summary, EntityId id) {
    for (const auto& c : summary.combatants) {
        if (c.entity_id == id) return &c;
    }
    return nullptr;
}

/// GCDs by when they were pressed (seconds into the pull), each stamped when the
/// client would see it: at the press for an instant, a cast time later for a hardcast.
struct Rotation {
    std::vector<GcdCast> casts;
    void instant(ActionId action, double press) { add(action, at(press)); }
    void hardcast(ActionId action, double press, double cast_s) { add(action, at(press + cast_s)); }

private:
    void add(ActionId action, uint64_t at_us) {
        const hub::game::GcdTiming timing = hub::game::gcd_timing(action);
        casts.push_back({at_us, timing.recast_100ms, timing.cast_100ms});
    }
};

} // namespace

TEST_CASE(MeterCasts, DecoderCountsOnlyPresses) {
    hub::game::ActionEffectHeader header{};
    header.action_type = decoder::ACTION_TYPE_ACTION;
    header.action_id = kHeavySwing;
    const auto pressed = decoder::decode_cast(kSam, header, at(3.0));
    TEST_ASSERT(pressed.has_value());
    TEST_ASSERT_EQ(pressed->source_id, kSam);
    TEST_ASSERT_EQ(pressed->action_id, kHeavySwing);
    TEST_ASSERT_EQ(pressed->timestamp_us, at(3.0));

    // Auto-attacks and what the game fires on its own are nobody's press.
    header.action_id = 7;
    TEST_ASSERT_FALSE(decoder::decode_cast(kSam, header, 0).has_value());
    header.action_id = kKardiaHeal;
    TEST_ASSERT_FALSE(decoder::decode_cast(kSam, header, 0).has_value());
    // A Limit Break has no cooldown group but is pressed all the same.
    header.action_id = kBraver;
    TEST_ASSERT(decoder::decode_cast(kSam, header, 0).has_value());
    // An item shares the id space.
    header.action_id = kHeavySwing;
    header.action_type = 2;
    TEST_ASSERT_FALSE(decoder::decode_cast(kSam, header, 0).has_value());
}

TEST_CASE(MeterGameData, GcdAndPressTablesComeFromTheSheet) {
    TEST_ASSERT(hub::game::action_sheet_name(kGlareIII) == "Glare III");
    TEST_ASSERT(hub::game::action_sheet_name(kCreatureMotif) == "Creature Motif");
    TEST_ASSERT(hub::game::action_sheet_name(kDrill) == "Drill");
    TEST_ASSERT(hub::game::action_sheet_name(kTen) == "Ten");
    TEST_ASSERT(hub::game::action_sheet_name(kKardiaHeal) == "Kardia");
    TEST_ASSERT(hub::game::action_sheet_name(kEudaimonia) == "Eudaimonia");

    // Drill, Ten and Standard Step keep their own cooldown and share the GCD as well.
    TEST_ASSERT(hub::game::is_gcd_action(kDrill));
    TEST_ASSERT(hub::game::is_gcd_action(kTen));
    TEST_ASSERT(hub::game::is_gcd_action(15997));
    TEST_ASSERT_FALSE(hub::game::is_gcd_action(kBerserk));
    TEST_ASSERT_FALSE(hub::game::is_gcd_action(3));  // Sprint

    TEST_ASSERT_EQ(hub::game::gcd_timing(kGlareIII).recast_100ms, 25u);
    TEST_ASSERT_EQ(hub::game::gcd_timing(kGlareIII).cast_100ms, 15u);
    TEST_ASSERT_EQ(hub::game::gcd_timing(kCreatureMotif).recast_100ms, 40u);
    TEST_ASSERT_EQ(hub::game::gcd_timing(kCreatureMotif).cast_100ms, 30u);
    TEST_ASSERT_EQ(hub::game::gcd_timing(kDrill).recast_100ms, 25u);
    TEST_ASSERT_EQ(hub::game::gcd_timing(kBerserk).recast_100ms, 0u);

    TEST_ASSERT(hub::game::is_pressed_action(3));
    TEST_ASSERT(hub::game::is_pressed_action(kBerserk));
    TEST_ASSERT(hub::game::is_pressed_action(kHeavySwing));
    TEST_ASSERT_FALSE(hub::game::is_pressed_action(7));
    TEST_ASSERT_FALSE(hub::game::is_pressed_action(8));
    TEST_ASSERT_FALSE(hub::game::is_pressed_action(kKardiaHeal));
    TEST_ASSERT_FALSE(hub::game::is_pressed_action(kEudaimonia));
}

TEST_CASE(MeterCasts, RollingInstantsAreFullUptime) {
    Rotation r;
    for (int k = 0; k < 25; ++k) r.instant(kHeavySwing, 2.4 * k);
    const GcdUptime result = gcd_uptime(r.casts, at(0.0), at(60.0));
    TEST_ASSERT_NEAR(result.estimate_s, 2.4, 1e-6);
    TEST_ASSERT_NEAR(result.uptime_pct, 100.0, 1e-6);
}

TEST_CASE(MeterCasts, FewGapsKeepTheBaseGcd) {
    Rotation r;
    for (int k = 0; k < 5; ++k) r.instant(kHeavySwing, 2.4 * k);
    const GcdUptime result = gcd_uptime(r.casts, at(0.0), at(12.0));
    TEST_ASSERT_NEAR(result.estimate_s, 2.5, 1e-9);
    TEST_ASSERT_NEAR(result.uptime_pct, 100.0, 1e-6);
    TEST_ASSERT_NEAR(gcd_uptime({}, at(0.0), at(12.0)).estimate_s, 0.0, 1e-9);
}

TEST_CASE(MeterCasts, ALateGcdCostsWhatItRanOver) {
    Rotation r;
    for (int k = 0; k <= 10; ++k) r.instant(kHeavySwing, 2.5 * k);
    for (int k = 0; k < 12; ++k) r.instant(kHeavySwing, 30.0 + 2.5 * k);
    // The 5 s gap counts 2.5 s and the slack: 2.4 s of 60 went idle.
    const GcdUptime result = gcd_uptime(r.casts, at(0.0), at(60.0));
    TEST_ASSERT_NEAR(result.estimate_s, 2.5, 1e-6);
    TEST_ASSERT_NEAR(result.uptime_pct, 96.0, 1e-6);
}

TEST_CASE(MeterCasts, HardcastsCountFromTheirPress) {
    Rotation r;
    for (int k = 0; k < 24; ++k) r.hardcast(kGlareIII, 2.5 * k, 1.5);
    // Counted from where each landed, the first 1.5 s would read as idle.
    const GcdUptime result = gcd_uptime(r.casts, at(0.0), at(60.0));
    TEST_ASSERT_NEAR(result.estimate_s, 2.5, 1e-6);
    TEST_ASSERT_NEAR(result.uptime_pct, 100.0, 1e-6);
}

TEST_CASE(MeterCasts, DualcastAlternationKeepsRolling) {
    // Hardcast, then the Dualcast instant: landing 0.5 s and 4.5 s apart.
    Rotation r;
    for (int k = 0; k < 12; ++k) {
        r.hardcast(kJoltIII, 5.0 * k, 2.0);
        r.instant(kVerthunderIII, 5.0 * k + 2.5);
    }
    const GcdUptime result = gcd_uptime(r.casts, at(0.0), at(60.0));
    TEST_ASSERT_NEAR(result.uptime_pct, 100.0, 1e-6);
}

TEST_CASE(MeterCasts, SwiftcastIsNotBackdated) {
    Rotation r;
    for (int k = 0; k < 24; ++k) {
        if (k == 2) {
            r.instant(kGlareIII, 5.0);
        } else {
            r.hardcast(kGlareIII, 2.5 * k, 1.5);
        }
    }
    const GcdUptime result = gcd_uptime(r.casts, at(0.0), at(60.0));
    TEST_ASSERT_NEAR(result.uptime_pct, 100.0, 1e-6);
}

TEST_CASE(MeterCasts, APrecastCountsOnlyInsideThePull) {
    // Pressed 1.5 s before the pull, which its hit opened.
    Rotation r;
    for (int k = 0; k < 24; ++k) r.hardcast(kGlareIII, -1.5 + 2.5 * k, 1.5);
    const GcdUptime result = gcd_uptime(r.casts, at(0.0), at(58.5));
    TEST_ASSERT_NEAR(result.uptime_pct, 100.0, 1e-6);
}

TEST_CASE(MeterCasts, ALongGcdHoldsItsOwnLength) {
    Rotation r;
    r.hardcast(kFireInRed, 0.0, 1.5);
    r.hardcast(kCreatureMotif, 2.5, 3.0);
    r.hardcast(kFireInRed, 6.5, 1.5);
    const GcdUptime result = gcd_uptime(r.casts, at(0.0), at(9.0));
    TEST_ASSERT_NEAR(result.uptime_pct, 100.0, 1e-6);
}

TEST_CASE(MeterCasts, ShortGcdsCountInFull) {
    Rotation r;
    r.instant(kSpinningEdge, 0.0);
    r.instant(kTen, 2.5);
    r.instant(kChi, 3.0);
    r.instant(kRaiton, 3.5);
    r.instant(kSpinningEdge, 5.0);
    const GcdUptime result = gcd_uptime(r.casts, at(0.0), at(7.5));
    TEST_ASSERT_NEAR(result.uptime_pct, 100.0, 1e-6);
}

TEST_CASE(MeterCasts, CastsNeverStartAPull) {
    EncounterEngine engine;
    register_party(engine.registry_unlocked());
    const Timeline tl;
    engine.process_cast(cast(kSam, kHeavySwing, 1.0), tl.at(1.0));
    TEST_ASSERT(engine.state() == EncounterState::Idle);
    TEST_ASSERT(engine.accumulator_unlocked().find_stats(kSam) == nullptr);

    // The press behind the hit that opened the pull counts; one stamped earlier does not.
    engine.process_action(hit(kSam, 10.0), tl.at(10.0));
    engine.process_cast(cast(kSam, kBerserk, 9.5), tl.at(10.0));
    engine.process_cast(cast(kSam, kHeavySwing, 10.0), tl.at(10.0));
    TEST_ASSERT_EQ(engine.accumulator_unlocked().find_stats(kSam)->casts, 1u);
}

TEST_CASE(MeterCasts, APlayersCastsKeepThePullAlive) {
    EncounterEngine engine(7.0);
    register_party(engine.registry_unlocked());
    const Timeline tl;
    engine.process_action(hit(kSam, 0.0), tl.at(0.0));
    engine.process_cast(cast(kSch, 16542, 5.0), tl.at(5.0));    // Recitation
    engine.process_cast(cast(kSch, 7436, 10.0), tl.at(10.0));   // Chain Stratagem
    engine.update(tl.at(14.0));
    TEST_ASSERT(engine.in_combat());
    engine.update(tl.at(17.5));
    TEST_ASSERT_FALSE(engine.in_combat());
    // The clock stops at the last cast, like at any other last activity.
    TEST_ASSERT_NEAR(engine.latest_pull()->duration_seconds, 10.0, 0.01);
}

TEST_CASE(MeterCasts, EnemyAndPetCastsAreNobodysPress) {
    EncounterEngine engine(7.0);
    register_party(engine.registry_unlocked());
    engine.registry_unlocked().register_actor(kEos, "Eos", Job::None, kSch, ActorType::Pet);
    const Timeline tl;
    engine.process_action(hit(kSam, 0.0), tl.at(0.0));
    engine.process_cast(cast(kEos, 16537, 1.0), tl.at(1.0));    // Whispering Dawn
    engine.process_cast(cast(kBoss, 7477, 5.0), tl.at(5.0));
    TEST_ASSERT(engine.accumulator_unlocked().find_stats(kSch) == nullptr);
    TEST_ASSERT(engine.accumulator_unlocked().find_stats(kBoss)->casts == 0u);
    // Neither kept the pull open.
    engine.update(tl.at(7.5));
    TEST_ASSERT_FALSE(engine.in_combat());
}

TEST_CASE(MeterCasts, CastsPerActionAndPerMinute) {
    EncounterEngine engine;
    register_party(engine.registry_unlocked());
    const Timeline tl;
    engine.process_action(hit(kSam, 0.0), tl.at(0.0));
    engine.process_cast(cast(kSam, kHeavySwing, 0.0), tl.at(0.0));
    engine.process_cast(cast(kSam, kBerserk, 1.0), tl.at(1.0));
    engine.process_cast(cast(kSam, kHeavySwing, 2.5), tl.at(2.5));
    engine.process_cast(cast(kSam, kHeavySwing, 5.0), tl.at(5.0));
    engine.end_encounter(EncounterEndReason::Manual, tl.at(60.0), at(60.0));

    const auto pull = engine.latest_pull();
    TEST_ASSERT(pull.has_value());
    const CombatantStats* sam = row_of(*pull, kSam);
    TEST_ASSERT(sam != nullptr);
    TEST_ASSERT_EQ(sam->casts, 4u);
    TEST_ASSERT_EQ(sam->gcd_casts, 3u);
    TEST_ASSERT_NEAR(sam->cpm, 4.0, 0.01);
    TEST_ASSERT_EQ(sam->actions.at(kHeavySwing).casts, 3u);
    TEST_ASSERT_EQ(sam->actions.at(kHeavySwing).hit_count, 1u);
    TEST_ASSERT_EQ(sam->actions.at(kBerserk).casts, 1u);
    // Three 2.5 s GCDs in a minute.
    TEST_ASSERT_NEAR(sam->gcd_estimate_s, 2.5, 1e-9);
    TEST_ASSERT_NEAR(sam->gcd_uptime_pct, 12.5, 0.05);
}

TEST_CASE(MeterCasts, APetMergedLateBringsNoCasts) {
    MetricsAccumulator acc;
    CombatantRegistry reg;
    register_party(reg);
    // Seen before anything linked it to its owner, under a name that gives nothing away.
    reg.register_actor(kEos, "Unlinked", Job::None, 0, ActorType::Player);
    TEST_ASSERT(acc.record_cast(cast(kEos, kHeavySwing, 1.0), reg));
    TEST_ASSERT(acc.record_cast(cast(kSch, 16537, 1.0), reg));

    reg.set_pet_owner(kEos, kSch);
    acc.recalculate(60.0, &reg);
    const CombatantStats* sch = acc.find_stats(kSch);
    TEST_ASSERT(sch != nullptr);
    TEST_ASSERT(acc.find_stats(kEos) == nullptr);
    TEST_ASSERT_EQ(sch->casts, 1u);
    TEST_ASSERT_EQ(sch->gcd_casts, 0u);
    TEST_ASSERT_EQ(sch->actions.at(kHeavySwing).casts, 0u);
    TEST_ASSERT_NEAR(sch->gcd_uptime_pct, 0.0, 1e-9);
}

TEST_CASE(MeterCasts, PluginSendsThePressAfterItsHits) {
    CombatPlugin plugin;
    plugin.initialize();
    hub::ipc::PacketRingBuffer ring;
    plugin.set_ring_buffer(&ring);
    register_party(plugin.engine().registry_unlocked());

    hub::game::ActionEffectHeader header{};
    header.animation_target_id = kBoss;
    header.action_id = 7477;  // Hakaze
    header.action_type = decoder::ACTION_TYPE_ACTION;
    header.num_targets = 1;
    std::array<hub::game::ActionEffectEntry, 8> entries{};
    entries[0].effect_type = 0x03;
    entries[0].value = 10500;
    plugin.on_receive_action_effect(kSam, nullptr, &header, entries.data(), nullptr);
    // An auto-attack hits but was never pressed.
    header.action_id = 7;
    plugin.on_receive_action_effect(kSam, nullptr, &header, entries.data(), nullptr);

    std::vector<hub::MessageType> order;
    std::vector<hub::ipc::CombatActionPacket> hits;
    std::vector<hub::ipc::CastPacket> casts;
    std::vector<uint8_t> frame;
    while (ring.pop(frame)) {
        const auto frame_header = hub::ipc::deserialize_header(frame);
        TEST_ASSERT(frame_header.has_value());
        const auto type = static_cast<hub::MessageType>(frame_header->message_type);
        const uint8_t* body = frame.data() + sizeof(hub::ipc::PacketHeader);
        if (type == hub::MessageType::CombatAction) {
            hub::ipc::CombatActionPacket packet{};
            std::memcpy(&packet, body, sizeof(packet));
            hits.push_back(packet);
        } else if (type == hub::MessageType::CombatCast) {
            hub::ipc::CastPacket packet{};
            std::memcpy(&packet, body, sizeof(packet));
            casts.push_back(packet);
        } else {
            continue;
        }
        order.push_back(type);
    }
    TEST_ASSERT_EQ(order.size(), 3u);
    TEST_ASSERT(order[0] == hub::MessageType::CombatAction);
    TEST_ASSERT(order[1] == hub::MessageType::CombatCast);
    TEST_ASSERT(order[2] == hub::MessageType::CombatAction);
    TEST_ASSERT_EQ(casts.size(), 1u);
    TEST_ASSERT_EQ(casts[0].source_id, kSam);
    TEST_ASSERT_EQ(casts[0].action_id, 7477u);
    TEST_ASSERT_EQ(casts[0].timestamp_us, hits[0].timestamp_us);

    // The app's engine, fed what was shipped in order, counts the same press.
    EncounterEngine mirror;
    register_party(mirror.registry_unlocked());
    mirror.process_action(hits[0]);
    mirror.process_cast(casts[0]);
    mirror.process_action(hits[1]);
    TEST_ASSERT_EQ(plugin.engine().accumulator_unlocked().find_stats(kSam)->casts, 1u);
    TEST_ASSERT_EQ(mirror.accumulator_unlocked().find_stats(kSam)->casts, 1u);
}

TEST_CASE(MeterCasts, WireStructSize) {
    TEST_ASSERT_EQ(sizeof(hub::ipc::CombatCastPayload), 16u);
    TEST_ASSERT_EQ(static_cast<uint16_t>(hub::MessageType::CombatCast), 0x020Au);
}
