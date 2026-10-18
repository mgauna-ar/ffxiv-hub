#include "test_framework.hpp"
#include "meter/combat_plugin.hpp"
#include "meter/downtime.hpp"
#include "meter/encounter_engine.hpp"
#include "meter/types.hpp"
#include "meter/vitals.hpp"
#include "common/ipc/ring_buffer.hpp"
#include <chrono>
#include <cstring>
#include <vector>

using namespace hub::meter;

namespace {

constexpr EntityId kTank = 0x10000001;
constexpr EntityId kHealer = 0x10000002;
constexpr EntityId kBoss = 0x40000001;
constexpr EntityId kAdd = 0x40000002;

constexpr uint64_t sec(double s) { return static_cast<uint64_t>(s * 1e6); }

/// Packets stamped sec(s) are processed at at(s).
struct Clock {
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    [[nodiscard]] std::chrono::steady_clock::time_point at(double s) const {
        return t0 + std::chrono::milliseconds(static_cast<int64_t>(s * 1000.0));
    }
};

hub::ipc::CombatActionPacket hit(EntityId source, EntityId target, uint32_t amount, double s,
                                 EffectType effect = EffectType::Damage) {
    hub::ipc::CombatActionPacket packet{};
    packet.source_id = source;
    packet.target_id = target;
    packet.action_id = effect == EffectType::Heal ? 120 : 31;
    packet.damage = amount;
    packet.effect_type = static_cast<uint16_t>(effect);
    packet.timestamp_us = sec(s);
    return packet;
}

hub::ipc::TargetabilityPacket report(bool targetable, double s) {
    hub::ipc::TargetabilityPacket packet{};
    packet.targetable = targetable ? 1 : 0;
    packet.timestamp_us = sec(s);
    return packet;
}

hub::ipc::EnemyHpPacket enemy_hp(EntityId enemy, uint32_t hp, uint32_t max_hp, double s) {
    hub::ipc::EnemyHpPacket packet{};
    packet.entity_id = enemy;
    packet.current_hp = hp;
    packet.max_hp = max_hp;
    packet.timestamp_us = sec(s);
    return packet;
}

/// A tank and a healer. With no game state reported, a quiet stretch ends a pull,
/// so the timeout is set past every gap these pulls have.
void setup_party(EncounterEngine& engine) {
    engine.set_inactivity_timeout(1000.0);
    hub::ipc::PartySyncPacket sync{};
    sync.entity_ids[0] = kTank;
    sync.job_ids[0] = static_cast<uint32_t>(Job::WAR);
    sync.entity_ids[1] = kHealer;
    sync.job_ids[1] = static_cast<uint32_t>(Job::WHM);
    sync.party_count = 2;
    engine.process_party_sync(sync);
    for (const EntityId id : {kTank, kHealer}) {
        hub::ipc::ActorInfoPacket info{};
        info.entity_id = id;
        info.job_id = static_cast<uint32_t>(id == kTank ? Job::WAR : Job::WHM);
        info.current_hp = 100;
        info.max_hp = 100;
        info.actor_type = static_cast<uint8_t>(ActorType::Player);
        engine.process_actor_info(info);
    }
}

ActorVitals enemy_vitals(EntityId id, uint32_t hp, bool targetable) {
    ActorVitals vitals{};
    vitals.entity = id;
    vitals.hp = hp;
    vitals.max_hp = 100000;
    vitals.is_enemy = true;
    vitals.targetable = targetable;
    return vitals;
}

std::vector<hub::ipc::TargetabilityPacket> drain_reports(hub::ipc::PacketRingBuffer& ring) {
    std::vector<hub::ipc::TargetabilityPacket> sent;
    std::vector<uint8_t> frame;
    while (ring.pop(frame)) {
        const auto header = hub::ipc::deserialize_header(frame);
        if (!header || static_cast<hub::MessageType>(header->message_type) != hub::MessageType::CombatTargetability) {
            continue;
        }
        hub::ipc::TargetabilityPacket packet{};
        std::memcpy(&packet, frame.data() + sizeof(hub::ipc::PacketHeader), sizeof(packet));
        sent.push_back(packet);
    }
    return sent;
}

const CombatantStats* row(const EncounterSummary& summary, EntityId id) {
    for (const CombatantStats& stats : summary.combatants) {
        if (stats.entity_id == id) return &stats;
    }
    return nullptr;
}

} // namespace

TEST_CASE(MeterDowntime, TrackerAddsClosedAndOpenStretches) {
    DowntimeTracker downtime;
    downtime.start(sec(10));
    downtime.observe(true, sec(12));    // Nothing open: nothing to close
    downtime.observe(false, sec(20));
    downtime.observe(false, sec(25));   // Still the stretch that began at 20
    downtime.observe(true, sec(30));
    downtime.observe(false, sec(50));
    TEST_ASSERT_NEAR(downtime.seconds_until(sec(60)), 20.0, 1e-9);
    // A clock stopped earlier leaves the rest out.
    TEST_ASSERT_NEAR(downtime.seconds_until(sec(25)), 5.0, 1e-9);
    TEST_ASSERT_NEAR(downtime.seconds_until(sec(15)), 0.0, 1e-9);
    downtime.clear();
    TEST_ASSERT_NEAR(downtime.seconds_until(sec(60)), 0.0, 1e-9);
}

TEST_CASE(MeterDowntime, TrackerDropsAReportFromBeforeThePull) {
    // The last pull's dead boss, reported just before this one opened.
    DowntimeTracker downtime;
    downtime.start(sec(10));
    downtime.observe(false, sec(9));
    TEST_ASSERT_NEAR(downtime.seconds_until(sec(60)), 0.0, 1e-9);
}

TEST_CASE(MeterDowntime, DamageRatesLeaveDowntimeOut) {
    // 100 s pull, 20 s of it with nothing to hit: FFLogs' DPS for this fight divides
    // by 80 s, and so must the meter's. Healing during the downtime was real work.
    EncounterEngine engine;
    setup_party(engine);
    const Clock t;
    engine.process_action(hit(kTank, kBoss, 40000, 1), t.at(1));
    engine.process_targetability(report(false, 31));
    engine.process_action(hit(kHealer, kTank, 10000, 40, EffectType::Heal), t.at(40));
    engine.process_targetability(report(true, 51));
    engine.process_action(hit(kTank, kBoss, 40000, 100), t.at(100));
    engine.end_encounter(EncounterEndReason::Manual, t.at(101));

    const auto summary = engine.latest_pull();
    TEST_ASSERT(summary.has_value());
    TEST_ASSERT_NEAR(summary->duration_seconds, 100.0, 1e-3);
    TEST_ASSERT_NEAR(summary->downtime_seconds, 20.0, 1e-3);
    TEST_ASSERT_NEAR(summary->total_dps, 80000.0 / 80.0, 1e-2);
    TEST_ASSERT_NEAR(summary->total_hps, 10000.0 / 100.0, 1e-2);
    const CombatantStats* tank = row(*summary, kTank);
    TEST_ASSERT(tank != nullptr);
    TEST_ASSERT_NEAR(tank->dps, 1000.0, 1e-2);
    TEST_ASSERT_NEAR(tank->rdps, 1000.0, 1e-2);
    TEST_ASSERT_NEAR(tank->adps, 1000.0, 1e-2);
}

TEST_CASE(MeterDowntime, LiveRatesCountAnOpenStretchSoFar) {
    EncounterEngine engine;
    setup_party(engine);
    const Clock t;
    engine.process_action(hit(kTank, kBoss, 30000, 1), t.at(1));
    engine.process_targetability(report(false, 21));
    const auto summary = engine.current_summary(t.at(31));
    TEST_ASSERT_NEAR(summary.downtime_seconds, 10.0, 1e-3);
    TEST_ASSERT_NEAR(summary.total_dps, 30000.0 / 20.0, 1e-2);
}

TEST_CASE(MeterDowntime, AKillLeavesOutTheDowntimeAfterIt) {
    // The pull's clock stops at the last hit on the dead boss, which reads
    // untargetable from then on.
    EncounterEngine engine;
    setup_party(engine);
    const Clock t;
    engine.process_action(hit(kTank, kBoss, 60000, 1), t.at(1));
    engine.process_action(hit(kTank, kBoss, 60000, 61), t.at(61));
    engine.process_enemy_hp(enemy_hp(kBoss, 0, 100000, 61.2));
    engine.process_targetability(report(false, 61.2));
    engine.end_encounter(EncounterEndReason::Manual, t.at(70));

    const auto summary = engine.latest_pull();
    TEST_ASSERT(summary.has_value());
    TEST_ASSERT(summary->boss.killed);
    TEST_ASSERT_NEAR(summary->duration_seconds, 60.0, 1e-3);
    TEST_ASSERT_NEAR(summary->downtime_seconds, 0.0, 1e-9);
    TEST_ASSERT_NEAR(summary->total_dps, 2000.0, 1e-2);
}

TEST_CASE(MeterDowntime, AllDowntimeFloorsAtOneSecond) {
    MetricsAccumulator accumulator;
    CombatantRegistry registry;
    hub::ipc::PartySyncPacket sync{};
    sync.entity_ids[0] = kTank;
    sync.job_ids[0] = static_cast<uint32_t>(Job::WAR);
    sync.party_count = 1;
    registry.sync_party(sync);
    accumulator.start(sec(1));
    accumulator.record_action(hit(kTank, kBoss, 5000, 1), registry);
    accumulator.recalculate(10.0, &registry, 12.0);
    TEST_ASSERT_NEAR(accumulator.total_dps(), 5000.0, 1e-9);
}

TEST_CASE(MeterDowntime, AReportThatBeatsTheOpeningHitStillCounts) {
    // The report travels on the orchestration lane and the hit on the main-thread
    // lane, so the app can see a report stamped inside the pull before the pull.
    EncounterEngine app;
    setup_party(app);
    const Clock t;
    app.process_targetability(report(false, 1.2));
    app.process_action(hit(kTank, kBoss, 10000, 1), t.at(1));
    app.process_targetability(report(true, 6.2));
    app.end_encounter(EncounterEndReason::Manual, t.at(21));
    TEST_ASSERT_NEAR(app.latest_pull()->downtime_seconds, 5.0, 1e-3);

    // One stamped before the next pull is the last pull's, and is dropped.
    app.process_targetability(report(false, 30));
    app.process_action(hit(kTank, kBoss, 10000, 31), t.at(31));
    app.end_encounter(EncounterEndReason::Manual, t.at(41));
    TEST_ASSERT_NEAR(app.latest_pull()->downtime_seconds, 0.0, 1e-9);
}

TEST_CASE(MeterDowntime, ResetForgetsTheDowntime) {
    EncounterEngine engine;
    setup_party(engine);
    const Clock t;
    engine.process_action(hit(kTank, kBoss, 10000, 1), t.at(1));
    engine.process_targetability(report(false, 2));
    engine.reset_current();
    engine.process_action(hit(kTank, kBoss, 10000, 10), t.at(10));
    TEST_ASSERT_NEAR(engine.current_summary(t.at(20)).downtime_seconds, 0.0, 1e-9);
}

TEST_CASE(MeterDowntime, PassReportsWhetherAnEnemyCanBeHitWhenThatChanges) {
    CombatPlugin plugin;
    plugin.initialize();
    hub::ipc::PacketRingBuffer ring;
    plugin.set_ring_buffer(&ring);
    EncounterEngine& engine = plugin.engine();
    setup_party(engine);
    const Clock t;
    engine.process_action(hit(kTank, kBoss, 1000, 1), t.at(1));

    std::vector<ActorVitals> pass{enemy_vitals(kBoss, 90000, true), enemy_vitals(kAdd, 5000, false)};
    plugin.on_vitals(pass, sec(1.25));
    auto sent = drain_reports(ring);
    TEST_ASSERT_EQ(sent.size(), 1u);
    TEST_ASSERT_EQ(sent[0].targetable, 1u);
    TEST_ASSERT_EQ(sent[0].timestamp_us, sec(1.25));

    plugin.on_vitals(pass, sec(1.5));
    TEST_ASSERT(drain_reports(ring).empty());

    // The boss jumps away; the add lying dead still reads targetable.
    pass[0].targetable = false;
    pass[1].hp = 0;
    pass[1].targetable = true;
    plugin.on_vitals(pass, sec(1.75));
    sent = drain_reports(ring);
    TEST_ASSERT_EQ(sent.size(), 1u);
    TEST_ASSERT_EQ(sent[0].targetable, 0u);

    // A pass that read no enemy says nothing, whatever it could not find.
    plugin.on_vitals({}, sec(2));
    TEST_ASSERT(drain_reports(ring).empty());

    // A listener that just connected gets the answer again.
    plugin.invalidate_published_vitals();
    plugin.on_vitals(pass, sec(2.25));
    TEST_ASSERT_EQ(drain_reports(ring).size(), 1u);

    pass[0].targetable = true;
    plugin.on_vitals(pass, sec(11.75));
    sent = drain_reports(ring);
    TEST_ASSERT_EQ(sent.size(), 1u);
    TEST_ASSERT_EQ(sent[0].targetable, 1u);
    TEST_ASSERT_NEAR(engine.current_summary(t.at(20)).downtime_seconds, 10.0, 1e-3);
}

TEST_CASE(MeterDowntime, NothingIsDowntimeUntilAnEnemyHasReadTargetable) {
    // A patch that moved the targetable byte reads every enemy as untargetable. Taken
    // at its word, every pull would be all downtime and its DPS divided by one second.
    CombatPlugin plugin;
    plugin.initialize();
    hub::ipc::PacketRingBuffer ring;
    plugin.set_ring_buffer(&ring);
    EncounterEngine& engine = plugin.engine();
    setup_party(engine);
    const Clock t;
    engine.process_action(hit(kTank, kBoss, 10000, 1), t.at(1));

    std::vector<ActorVitals> pass{enemy_vitals(kBoss, 90000, false)};
    plugin.on_vitals(pass, sec(1.25));
    plugin.on_vitals(pass, sec(5));
    TEST_ASSERT(drain_reports(ring).empty());
    TEST_ASSERT_NEAR(engine.current_summary(t.at(11)).downtime_seconds, 0.0, 1e-9);

    // Once it has read set, an unset bit is believed.
    pass[0].targetable = true;
    plugin.on_vitals(pass, sec(11));
    pass[0].targetable = false;
    plugin.on_vitals(pass, sec(12));
    const auto sent = drain_reports(ring);
    TEST_ASSERT_EQ(sent.size(), 2u);
    TEST_ASSERT_EQ(sent[1].targetable, 0u);
}

TEST_CASE(MeterDowntime, PassWithTrackingOffSaysNothing) {
    CombatPlugin plugin;
    plugin.initialize();
    hub::ipc::PacketRingBuffer ring;
    plugin.set_ring_buffer(&ring);
    setup_party(plugin.engine());
    plugin.set_vitals_tracking(false);
    std::vector<ActorVitals> pass{enemy_vitals(kBoss, 90000, false)};
    plugin.on_vitals(pass, sec(1));
    TEST_ASSERT(drain_reports(ring).empty());
}

TEST_CASE(MeterDowntime, BothEnginesAgreeOnTheRates) {
    CombatPlugin plugin;
    plugin.initialize();
    hub::ipc::PacketRingBuffer ring;
    plugin.set_ring_buffer(&ring);
    EncounterEngine app;
    setup_party(plugin.engine());
    setup_party(app);
    const Clock t;

    const auto both = [&](const hub::ipc::CombatActionPacket& packet, double s) {
        plugin.engine().process_action(packet, t.at(s));
        app.process_action(packet, t.at(s));
    };
    const auto pass = [&](bool targetable, double s) {
        std::vector<ActorVitals> vitals{enemy_vitals(kBoss, 50000, targetable)};
        plugin.on_vitals(vitals, sec(s));
        for (const auto& sent : drain_reports(ring)) app.process_targetability(sent);
    };
    both(hit(kTank, kBoss, 20000, 1), 1);
    pass(true, 1.25);
    pass(false, 11.25);
    pass(true, 26.25);
    both(hit(kTank, kBoss, 20000, 41), 41);

    const auto in_game = plugin.engine().current_summary(t.at(41));
    const auto desktop = app.current_summary(t.at(41));
    TEST_ASSERT_NEAR(in_game.downtime_seconds, 15.0, 1e-3);
    TEST_ASSERT_NEAR(desktop.downtime_seconds, in_game.downtime_seconds, 1e-9);
    TEST_ASSERT_NEAR(in_game.total_dps, 40000.0 / 25.0, 1e-2);
    TEST_ASSERT_NEAR(desktop.total_dps, in_game.total_dps, 1e-9);
}
