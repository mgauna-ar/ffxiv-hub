#include "test_framework.hpp"
#include "meter/combat_plugin.hpp"
#include "meter/encounter_engine.hpp"
#include "meter/metrics_accumulator.hpp"
#include "meter/status_uptime.hpp"
#include "meter/timeline.hpp"
#include <chrono>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <vector>

using namespace hub::meter;

namespace {

constexpr EntityId kWar = 0x10000001;
constexpr EntityId kDrg = 0x10000002;
constexpr EntityId kSch = 0x10000003;
constexpr EntityId kStranger = 0x10000009;  // A player outside the party
constexpr EntityId kEos = 0x40000100;
constexpr EntityId kCarbuncle = 0x40000101;
constexpr EntityId kBoss = 0x40000001;

constexpr uint16_t kBattleLitany = 786;     // On the party, from the dragoon
constexpr uint16_t kChainStratagem = 1221;  // On the enemy, from the scholar
constexpr uint16_t kTheBalance = 3887;      // On one player
constexpr uint16_t kInnerRelease = 1177;    // The warrior's own
constexpr ActionId kBraver = 200;           // Limit Break

/// A pull a long way into the steady clock: a packet stamped 0 means "unstamped".
constexpr uint64_t kT0 = 1'000'000'000;

uint64_t at(double s) {
    return kT0 + static_cast<uint64_t>(std::llround(s * 1e6));
}

/// The engine's clock in step with the packets: the packet stamped at(s) is
/// processed at clock(s).
struct Clock {
    std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    [[nodiscard]] std::chrono::steady_clock::time_point operator()(double s) const {
        return t0 + std::chrono::microseconds(std::llround(s * 1e6));
    }
};

hub::ipc::CombatActionPacket hit(EntityId source, EntityId target, uint32_t amount, double s) {
    hub::ipc::CombatActionPacket packet{};
    packet.source_id = source;
    packet.target_id = target;
    packet.action_id = 31;
    packet.damage = amount;
    packet.effect_type = static_cast<uint16_t>(EffectType::Damage);
    packet.timestamp_us = at(s);
    return packet;
}

hub::ipc::CombatActionPacket heal(EntityId source, EntityId target, uint32_t effective, uint32_t overheal, double s) {
    hub::ipc::CombatActionPacket packet{};
    packet.source_id = source;
    packet.target_id = target;
    packet.action_id = 185;
    packet.effective_heal = effective;
    packet.overheal = overheal;
    packet.effect_type = static_cast<uint16_t>(EffectType::Heal);
    packet.timestamp_us = at(s);
    return packet;
}

hub::ipc::StatusTickPacket tick(EntityId source, EntityId target, uint32_t amount, double s, EffectType effect,
                                uint32_t overheal = 0) {
    hub::ipc::StatusTickPacket packet{};
    packet.source_id = source;
    packet.target_id = target;
    packet.status_id = 1895;
    packet.damage_or_heal = amount;
    packet.overheal = overheal;
    packet.effect_type = static_cast<uint8_t>(effect);
    packet.timestamp_us = at(s);
    return packet;
}

void add_credit(hub::ipc::CombatActionPacket& packet, EntityId giver, uint32_t amount, bool single) {
    hub::ipc::CombatBuffCredit& credit = packet.credits.entries[packet.credits.count++];
    credit.giver_id = giver;
    credit.amount = amount;
    credit.single_target = single ? 1 : 0;
}

/// `entity`'s whole status list at `s`; timers are left at 0 so a loss is dated to the read.
hub::ipc::StatusListPacket statuses(EntityId entity, double s,
                                    std::initializer_list<std::pair<uint16_t, EntityId>> entries) {
    hub::ipc::StatusListPacket packet{};
    packet.entity_id = entity;
    packet.timestamp_us = at(s);
    for (const auto& [status, source] : entries) {
        hub::ipc::CombatStatusEntry& entry = packet.entries[packet.count++];
        entry.status_id = status;
        entry.source_id = source;
    }
    return packet;
}

hub::ipc::ActorInfoPacket actor_info(EntityId id, const char* name, ActorType type, Job job) {
    hub::ipc::ActorInfoPacket info{};
    info.entity_id = id;
    info.job_id = static_cast<uint32_t>(job);
    info.current_hp = 100;
    info.max_hp = 100;
    info.actor_type = static_cast<uint8_t>(type);
    std::strncpy(info.name, name, sizeof(info.name) - 1);
    return info;
}

/// A warrior, a dragoon and a scholar with her fairy in the party, a player outside
/// it, and a boss.
void setup(EncounterEngine& engine) {
    engine.set_timeline_enabled(true);
    hub::ipc::PartySyncPacket sync{};
    const EntityId party[] = {kWar, kDrg, kSch};
    const Job jobs[] = {Job::WAR, Job::DRG, Job::SCH};
    for (size_t i = 0; i < 3; ++i) {
        sync.entity_ids[i] = party[i];
        sync.job_ids[i] = static_cast<uint32_t>(jobs[i]);
    }
    sync.party_count = 3;
    sync.local_player_id = kWar;
    engine.process_party_sync(sync);
    engine.process_actor_info(actor_info(kWar, "War", ActorType::Player, Job::WAR));
    engine.process_actor_info(actor_info(kDrg, "Drg", ActorType::Player, Job::DRG));
    engine.process_actor_info(actor_info(kSch, "Sch", ActorType::Player, Job::SCH));
    engine.process_actor_info(actor_info(kStranger, "Stranger", ActorType::Player, Job::NIN));
    hub::ipc::ActorInfoPacket eos = actor_info(kEos, "Eos", ActorType::Pet, Job::None);
    eos.owner_id = kSch;
    engine.process_actor_info(eos);
    engine.process_actor_info(actor_info(kBoss, "Boss", ActorType::Monster, Job::None));
}

void register_party(CombatantRegistry& registry) {
    registry.set_party_members({kWar, kDrg, kSch});
    registry.register_actor(kWar, "War", Job::WAR, 0, ActorType::Player);
    registry.register_actor(kDrg, "Drg", Job::DRG, 0, ActorType::Player);
    registry.register_actor(kSch, "Sch", Job::SCH, 0, ActorType::Player);
    registry.register_actor(kBoss, "Boss", Job::None, 0, ActorType::Monster);
}

const TimelineRow* row_of(const EncounterTimeline& timeline, EntityId id) {
    for (const TimelineRow& row : timeline.rows) {
        if (row.entity == id) return &row;
    }
    return nullptr;
}

const TimelineRow* row_of(const std::vector<TimelineRow>& rows, EntityId id) {
    for (const TimelineRow& row : rows) {
        if (row.entity == id) return &row;
    }
    return nullptr;
}

} // namespace

TEST_CASE(MeterTimeline, BinsFollowTheSecondEachHitLanded) {
    EncounterEngine engine;
    setup(engine);
    const Clock clock;
    engine.process_action(hit(kWar, kBoss, 1000, 0.0), clock(0.0));
    engine.process_action(hit(kWar, kBoss, 500, 0.9), clock(0.9));
    engine.process_action(hit(kWar, kBoss, 700, 1.2), clock(1.2));
    engine.process_action(hit(kWar, kBoss, 300, 5.5), clock(5.5));

    const EncounterTimeline live = engine.timeline(0, clock(6.0));
    const TimelineRow* war = row_of(live, kWar);
    TEST_ASSERT(war != nullptr);
    TEST_ASSERT_EQ(war->bins.size(), 6u);
    TEST_ASSERT_EQ(war->bins[0].damage, 1500u);
    TEST_ASSERT_EQ(war->bins[1].damage, 700u);
    TEST_ASSERT_EQ(war->bins[3].damage, 0u);
    TEST_ASSERT_EQ(war->bins[5].damage, 300u);
}

TEST_CASE(MeterTimeline, EachSeriesKeepsItsOwnSide) {
    EncounterEngine engine;
    setup(engine);
    const Clock clock;
    engine.process_action(hit(kWar, kBoss, 1000, 0.0), clock(0.0));
    engine.process_action(hit(kBoss, kWar, 4000, 1.5), clock(1.5));
    engine.process_action(heal(kSch, kWar, 3000, 500, 2.0), clock(2.0));
    engine.process_status_tick(tick(kSch, kBoss, 800, 3.0, EffectType::Damage), clock(3.0));
    engine.process_status_tick(tick(kSch, kWar, 600, 3.5, EffectType::Heal, 100), clock(3.5));
    // The fairy heals for her scholar.
    engine.process_action(heal(kEos, kWar, 900, 0, 4.2), clock(4.2));

    const EncounterTimeline live = engine.timeline(0, clock(5.0));
    const TimelineRow* war = row_of(live, kWar);
    const TimelineRow* sch = row_of(live, kSch);
    TEST_ASSERT(war != nullptr);
    TEST_ASSERT(sch != nullptr);
    TEST_ASSERT_EQ(war->bins[1].taken, 4000u);
    TEST_ASSERT_EQ(war->bins[1].damage, 0u);
    // Overheal is left out, from a hit and from a tick alike.
    TEST_ASSERT_EQ(sch->bins[2].healing, 3000u);
    TEST_ASSERT_EQ(sch->bins[3].damage, 800u);
    TEST_ASSERT_EQ(sch->bins[3].healing, 500u);
    TEST_ASSERT_EQ(sch->bins[4].healing, 900u);
    TEST_ASSERT(row_of(live, kEos) == nullptr);
    TEST_ASSERT(row_of(live, kBoss) == nullptr);
}

TEST_CASE(MeterTimeline, CreditsLandInTheSecondOfTheirHit) {
    EncounterEngine engine;
    setup(engine);
    const Clock clock;
    engine.process_action(hit(kDrg, kBoss, 2000, 0.0), clock(0.0));
    hub::ipc::CombatActionPacket buffed = hit(kWar, kBoss, 10000, 2.3);
    add_credit(buffed, kDrg, 500, false);
    add_credit(buffed, kSch, 300, true);
    engine.process_action(buffed, clock(2.3));

    const EncounterTimeline live = engine.timeline(0, clock(3.0));
    const TimelineBin& war = row_of(live, kWar)->bins[2];
    const TimelineBin& drg = row_of(live, kDrg)->bins[2];
    const TimelineBin& sch = row_of(live, kSch)->bins[2];
    TEST_ASSERT_EQ(war.buff_received, 800u);
    TEST_ASSERT_EQ(war.buff_received_single, 300u);
    TEST_ASSERT_EQ(drg.buff_given, 500u);
    TEST_ASSERT_EQ(sch.buff_given, 300u);

    const auto damage = [](const TimelineBin& bin, DpsMetric dps) {
        return timeline_value(bin, TimelineMetric::Damage, dps);
    };
    TEST_ASSERT_NEAR(damage(war, DpsMetric::Dps), 10000.0, 1e-9);
    TEST_ASSERT_NEAR(damage(war, DpsMetric::Rdps), 9200.0, 1e-9);
    TEST_ASSERT_NEAR(damage(war, DpsMetric::Adps), 9700.0, 1e-9);
    TEST_ASSERT_NEAR(damage(war, DpsMetric::Ndps), 9200.0, 1e-9);
    TEST_ASSERT_NEAR(damage(war, DpsMetric::Cdps), 9700.0, 1e-9);
    TEST_ASSERT_NEAR(damage(drg, DpsMetric::Rdps), 500.0, 1e-9);
    // Credits move damage between players and never make any: second by second, the
    // party's rDPS adds up to its DPS.
    for (size_t second = 0; second < 3; ++second) {
        double dps = 0.0;
        double rdps = 0.0;
        for (const TimelineRow& row : live.rows) {
            if (second >= row.bins.size()) continue;
            dps += damage(row.bins[second], DpsMetric::Dps);
            rdps += damage(row.bins[second], DpsMetric::Rdps);
        }
        TEST_ASSERT_NEAR(rdps, dps, 1e-9);
    }
}

TEST_CASE(MeterTimeline, OnlyThePartyKeepsATimeline) {
    EncounterEngine engine;
    setup(engine);
    const Clock clock;
    engine.process_action(hit(kWar, kBoss, 1000, 0.0), clock(0.0));
    engine.process_action(hit(kStranger, kBoss, 1000, 0.5), clock(0.5));
    hub::ipc::CombatActionPacket braver = hit(kWar, kBoss, 90000, 1.0);
    braver.action_id = kBraver;
    engine.process_action(braver, clock(1.0));

    const EncounterTimeline live = engine.timeline(0, clock(2.0));
    TEST_ASSERT_EQ(live.rows.size(), 1u);
    const TimelineRow* war = row_of(live, kWar);
    TEST_ASSERT(war != nullptr);
    // The Limit Break is the party's, on no one's line.
    TEST_ASSERT_EQ(war->bins.size(), 1u);
    TEST_ASSERT_EQ(war->bins[0].damage, 1000u);
    TEST_ASSERT(row_of(live, kStranger) == nullptr);
}

TEST_CASE(MeterTimeline, SoloTheLocalPlayerKeepsOne) {
    MetricsAccumulator acc;
    CombatantRegistry reg;
    reg.register_actor(kDrg, "Drg", Job::DRG, 0, ActorType::Player);
    reg.set_local_player(kDrg);
    acc.set_timeline_enabled(true);
    acc.start(at(0.0));
    acc.record_action(hit(kDrg, kBoss, 1200, 0.4), reg);

    const auto rows = acc.timeline_rows();
    TEST_ASSERT_EQ(rows.size(), 1u);
    TEST_ASSERT_EQ(rows[0].entity, kDrg);
    TEST_ASSERT_EQ(rows[0].bins[0].damage, 1200u);
}

TEST_CASE(MeterTimeline, APetsSecondsJoinItsOwner) {
    MetricsAccumulator acc;
    CombatantRegistry reg;
    register_party(reg);
    // Known for a pet by its name, with no summoner in the party to give it an owner.
    reg.register_actor(kCarbuncle, "Carbuncle", Job::None, 0, ActorType::Pet);
    TEST_ASSERT(reg.is_pet(kCarbuncle));
    TEST_ASSERT_EQ(reg.resolve_owner(kCarbuncle), kCarbuncle);
    acc.set_timeline_enabled(true);
    acc.start(at(0.0));
    acc.record_action(hit(kSch, kBoss, 1000, 0.5), reg);
    acc.record_action(hit(kCarbuncle, kBoss, 400, 0.7), reg);
    acc.record_action(hit(kCarbuncle, kBoss, 600, 2.1), reg);
    TEST_ASSERT(row_of(acc.timeline_rows(), kCarbuncle) != nullptr);

    reg.set_pet_owner(kCarbuncle, kSch);
    acc.recalculate(3.0, &reg);
    const auto rows = acc.timeline_rows();
    TEST_ASSERT(row_of(rows, kCarbuncle) == nullptr);
    const TimelineRow* sch = row_of(rows, kSch);
    TEST_ASSERT(sch != nullptr);
    TEST_ASSERT_EQ(sch->bins.size(), 3u);
    TEST_ASSERT_EQ(sch->bins[0].damage, 1400u);
    TEST_ASSERT_EQ(sch->bins[2].damage, 600u);
}

TEST_CASE(MeterTimeline, ATimelineStopsAtAnHour) {
    MetricsAccumulator acc;
    CombatantRegistry reg;
    register_party(reg);
    acc.set_timeline_enabled(true);
    acc.start(at(0.0));
    acc.record_action(hit(kWar, kBoss, 100, 0.0), reg);
    acc.record_action(hit(kWar, kBoss, 200, 3599.5), reg);
    acc.record_action(hit(kWar, kBoss, 300, 3600.0), reg);
    acc.record_action(hit(kWar, kBoss, 400, 5000.0), reg);

    const auto rows = acc.timeline_rows();
    TEST_ASSERT_EQ(rows[0].bins.size(), TIMELINE_MAX_SECONDS);
    TEST_ASSERT_EQ(rows[0].bins.back().damage, 200u);
    // The totals still count all of it.
    TEST_ASSERT_EQ(acc.find_stats(kWar)->total_damage, 1000u);
}

TEST_CASE(MeterTimeline, OffUnlessTheAppTurnsItOn) {
    EncounterEngine engine;
    setup(engine);
    engine.set_timeline_enabled(false);
    const Clock clock;
    engine.process_action(hit(kWar, kBoss, 1000, 0.0), clock(0.0));
    TEST_ASSERT(engine.timeline(0, clock(1.0)).rows.empty());
    engine.end_encounter(EncounterEndReason::Manual, clock(1.0), at(1.0));
    TEST_ASSERT(engine.timeline(engine.pull_history_index().back().encounter_id).rows.empty());

    // The in-game engine never turns it on.
    CombatPlugin plugin;
    register_party(plugin.engine().registry());
    plugin.engine().process_action(hit(kWar, kBoss, 1000, 0.0), clock(0.0));
    TEST_ASSERT(plugin.engine().timeline(0, clock(1.0)).rows.empty());
}

TEST_CASE(MeterTimeline, AnArchivedPullKeepsItsOwn) {
    EncounterEngine engine;
    setup(engine);
    engine.set_history_capacity(2);
    // In a zone: an unknown zone keeps only its newest pull.
    engine.set_zone(1238);
    const Clock clock;
    for (int pull = 0; pull < 3; ++pull) {
        const double s = 20.0 * pull;
        engine.process_action(hit(kWar, kBoss, 1000u * static_cast<uint32_t>(pull + 1), s), clock(s));
        engine.end_encounter(EncounterEndReason::Manual, clock(s + 2.0), at(s + 2.0));
    }

    const auto history = engine.pull_history_index();
    TEST_ASSERT_EQ(history.size(), 2u);
    TEST_ASSERT_EQ(row_of(engine.timeline(history[0].encounter_id), kWar)->bins[0].damage, 2000u);
    TEST_ASSERT_EQ(row_of(engine.timeline(history[1].encounter_id), kWar)->bins[0].damage, 3000u);
    // The first pull went with its summary.
    TEST_ASSERT(engine.timeline(history[0].encounter_id - 1).rows.empty());
    // Until the next pull starts, the live view still shows the one that ended.
    TEST_ASSERT_EQ(row_of(engine.timeline(0, clock(63.0)), kWar)->bins[0].damage, 3000u);

    engine.process_action(hit(kWar, kBoss, 4000, 70.0), clock(70.0));
    const EncounterTimeline live = engine.timeline(0, clock(71.0));
    TEST_ASSERT_EQ(row_of(live, kWar)->bins.size(), 1u);
    TEST_ASSERT_EQ(row_of(live, kWar)->bins[0].damage, 4000u);
    TEST_ASSERT_EQ(row_of(engine.timeline(history[1].encounter_id), kWar)->bins[0].damage, 3000u);
}

TEST_CASE(MeterTimeline, RaidBuffWindowsMergeOverTheParty) {
    CombatantRegistry reg;
    register_party(reg);
    StatusUptime uptime;
    uptime.start(at(0.0), reg);
    std::vector<StatusChange> changes;
    const auto apply = [&](const hub::ipc::StatusListPacket& list) {
        changes.clear();
        reg.apply_status_list(list, changes);
        uptime.apply(changes);
    };
    // Battle Litany reaches each list a little apart, as each is read.
    apply(statuses(kWar, 10.0, {{kBattleLitany, kDrg}, {kInnerRelease, kWar}, {kTheBalance, kSch}}));
    apply(statuses(kDrg, 10.2, {{kBattleLitany, kDrg}}));
    apply(statuses(kSch, 10.3, {{kBattleLitany, kDrg}}));
    apply(statuses(kWar, 30.0, {}));
    apply(statuses(kDrg, 30.1, {}));
    apply(statuses(kSch, 30.3, {}));
    apply(statuses(kBoss, 40.0, {{kChainStratagem, kSch}}));
    apply(statuses(kBoss, 55.0, {}));
    // Two minutes on, a window of its own. A read that lost it for half a second
    // does not split it.
    apply(statuses(kWar, 130.0, {{kBattleLitany, kDrg}}));
    apply(statuses(kWar, 140.0, {}));
    apply(statuses(kWar, 140.5, {{kBattleLitany, kDrg}}));
    apply(statuses(kWar, 150.0, {}));
    uptime.stop(at(200.0));

    const std::vector<BuffWindow> windows = uptime.windows(at(200.0), reg);
    TEST_ASSERT_EQ(windows.size(), 3u);
    TEST_ASSERT_EQ(windows[0].status, kBattleLitany);
    TEST_ASSERT_EQ(windows[0].source, kDrg);
    TEST_ASSERT_NEAR(windows[0].begin_s, 10.0, 1e-9);
    TEST_ASSERT_NEAR(windows[0].end_s, 30.3, 1e-9);
    TEST_ASSERT_EQ(windows[1].status, kChainStratagem);
    TEST_ASSERT_EQ(windows[1].source, kSch);
    TEST_ASSERT_NEAR(windows[1].begin_s, 40.0, 1e-9);
    TEST_ASSERT_NEAR(windows[1].end_s, 55.0, 1e-9);
    TEST_ASSERT_NEAR(windows[2].begin_s, 130.0, 1e-9);
    TEST_ASSERT_NEAR(windows[2].end_s, 150.0, 1e-9);
}

TEST_CASE(MeterTimeline, ALiveWindowRunsToNow) {
    EncounterEngine engine;
    setup(engine);
    const Clock clock;
    engine.process_action(hit(kWar, kBoss, 1000, 0.0), clock(0.0));
    engine.process_status_list(statuses(kWar, 5.0, {{kBattleLitany, kDrg}}));

    const EncounterTimeline live = engine.timeline(0, clock(12.0));
    TEST_ASSERT_EQ(live.buffs.size(), 1u);
    TEST_ASSERT_NEAR(live.buffs[0].begin_s, 5.0, 1e-9);
    TEST_ASSERT_NEAR(live.buffs[0].end_s, 12.0, 1e-3);

    engine.end_encounter(EncounterEndReason::Manual, clock(20.0), at(20.0));
    const EncounterTimeline archived = engine.timeline(engine.pull_history_index().back().encounter_id);
    TEST_ASSERT_EQ(archived.buffs.size(), 1u);
    TEST_ASSERT_NEAR(archived.buffs[0].end_s, 20.0, 1e-9);
}

TEST_CASE(MeterTimeline, SmoothingWeighsTheMiddleOfItsWindow) {
    std::vector<TimelineBin> bins(7);
    bins[3].damage = 90;
    const std::vector<float> smoothed = smoothed_series(bins, 7, TimelineMetric::Damage, DpsMetric::Dps, 5);
    TEST_ASSERT_EQ(smoothed.size(), 7u);
    // Five seconds weigh 1, 2, 3, 2, 1: the hit counts most where it landed.
    TEST_ASSERT_NEAR(smoothed[3], 30.0, 1e-4);
    TEST_ASSERT_NEAR(smoothed[2], 20.0, 1e-4);
    TEST_ASSERT_NEAR(smoothed[4], 20.0, 1e-4);
    TEST_ASSERT_NEAR(smoothed[1], 10.0, 1e-4);
    TEST_ASSERT_NEAR(smoothed[0], 0.0, 1e-4);

    // A steady rate reads the same all the way along, the edges included, where
    // the window holds fewer seconds and averages only those.
    std::vector<TimelineBin> steady(40);
    for (TimelineBin& bin : steady) bin.damage = 1000;
    for (const size_t window : {5u, 15u, 30u}) {
        for (const float v : smoothed_series(steady, 40, TimelineMetric::Damage, DpsMetric::Dps, window)) {
            TEST_ASSERT_NEAR(v, 1000.0, 1e-3);
        }
    }

    // Seconds past the last bin had nothing in them.
    std::vector<TimelineBin> short_bins(2);
    short_bins[0].healing = 10;
    short_bins[1].healing = 10;
    const std::vector<float> padded = smoothed_series(short_bins, 4, TimelineMetric::Healing, DpsMetric::Dps, 1);
    TEST_ASSERT_EQ(padded.size(), 4u);
    TEST_ASSERT_NEAR(padded[1], 10.0, 1e-6);
    TEST_ASSERT_NEAR(padded[3], 0.0, 1e-6);
}

TEST_CASE(MeterTimeline, DownsampleAveragesEachSpan) {
    const std::vector<float> seven{1, 3, 5, 7, 9, 11, 13};
    const std::vector<float> three = downsample(seven, 3);
    TEST_ASSERT_EQ(three.size(), 3u);
    TEST_ASSERT_NEAR(three[0], 2.0, 1e-6);   // 1, 3
    TEST_ASSERT_NEAR(three[1], 6.0, 1e-6);   // 5, 7
    TEST_ASSERT_NEAR(three[2], 11.0, 1e-6);  // 9, 11, 13
    // Fewer values than points are kept as they are.
    TEST_ASSERT_EQ(downsample(seven, 10).size(), 7u);
    const std::vector<float> flat(TIMELINE_MAX_SECONDS, 42.0f);
    for (const float v : downsample(flat, 700)) TEST_ASSERT_NEAR(v, 42.0, 1e-4);
}
