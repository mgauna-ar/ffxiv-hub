#include "test_framework.hpp"
#include "meter/types.hpp"
#include "meter/encounter_engine.hpp"
#include "meter/combat_plugin.hpp"
#include "meter/vitals.hpp"
#include <chrono>
#include <cstring>
#include <initializer_list>
#include <vector>

using namespace hub::meter;

namespace {

constexpr EntityId kTank = 0x10000001;
constexpr EntityId kHealer = 0x10000002;
constexpr EntityId kBoss = 0x40000001;
constexpr EntityId kAdd = 0x40000002;
constexpr uint32_t kBossMaxHp = 100000;

constexpr uint64_t sec(double s) { return static_cast<uint64_t>(s * 1e6); }

/// A pull whose packets are stamped in seconds, with the engine's clock in step:
/// the packet stamped sec(s) is processed at at(s).
struct Timeline {
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

hub::ipc::StatusTickPacket hot_tick(EntityId source, EntityId target, uint32_t amount, double s) {
    hub::ipc::StatusTickPacket tick{};
    tick.source_id = source;
    tick.target_id = target;
    tick.status_id = 158;  // Regen
    tick.damage_or_heal = amount;
    tick.effect_type = static_cast<uint8_t>(EffectType::Heal);
    tick.timestamp_us = sec(s);
    return tick;
}

hub::ipc::EnemyHpPacket enemy_hp(EntityId enemy, uint32_t hp, uint32_t max_hp, double s) {
    hub::ipc::EnemyHpPacket packet{};
    packet.entity_id = enemy;
    packet.current_hp = hp;
    packet.max_hp = max_hp;
    packet.timestamp_us = sec(s);
    return packet;
}

hub::ipc::ActorInfoPacket actor_info(EntityId id, const char* name, ActorType type, Job job,
                                     uint32_t hp, uint32_t max_hp) {
    hub::ipc::ActorInfoPacket info{};
    info.entity_id = id;
    info.job_id = static_cast<uint32_t>(job);
    info.current_hp = hp;
    info.max_hp = max_hp;
    info.actor_type = static_cast<uint8_t>(type);
    std::strncpy(info.name, name, sizeof(info.name) - 1);
    return info;
}

/// A tank and a healer, both alive, and a boss the registry knows by name.
void setup_party(EncounterEngine& engine) {
    engine.set_inactivity_timeout(7.0);
    hub::ipc::PartySyncPacket sync{};
    sync.entity_ids[0] = kTank;
    sync.job_ids[0] = static_cast<uint32_t>(Job::WAR);
    sync.entity_ids[1] = kHealer;
    sync.job_ids[1] = static_cast<uint32_t>(Job::WHM);
    sync.party_count = 2;
    engine.process_party_sync(sync);
    engine.process_actor_info(actor_info(kTank, "Tank", ActorType::Player, Job::WAR, 100, 100));
    engine.process_actor_info(actor_info(kHealer, "Healer", ActorType::Player, Job::WHM, 100, 100));
    engine.process_actor_info(actor_info(kBoss, "Titan", ActorType::Monster, Job::None, kBossMaxHp, kBossMaxHp));
}

void wipe(EncounterEngine& engine) {
    engine.process_actor_info(actor_info(kTank, "Tank", ActorType::Player, Job::WAR, 0, 100));
    engine.process_actor_info(actor_info(kHealer, "Healer", ActorType::Player, Job::WHM, 0, 100));
}

} // namespace

TEST_CASE(MeterBoss, OutcomeRules) {
    BossSummary unknown;
    TEST_ASSERT(pull_outcome(EncounterState::Complete, unknown) == PullOutcome::Clear);  // As before HP was read
    TEST_ASSERT(pull_outcome(EncounterState::Wipe, unknown) == PullOutcome::Wipe);

    BossSummary standing;
    standing.hp_pct = 41.0;
    TEST_ASSERT(pull_outcome(EncounterState::Complete, standing) == PullOutcome::Ended);
    TEST_ASSERT(pull_outcome(EncounterState::Wipe, standing) == PullOutcome::Wipe);

    BossSummary killed;
    killed.hp_pct = 0.0;
    killed.killed = true;
    TEST_ASSERT(pull_outcome(EncounterState::Complete, killed) == PullOutcome::Clear);
    // The boss died with the party: it still went down.
    TEST_ASSERT(pull_outcome(EncounterState::Wipe, killed) == PullOutcome::Clear);
}

TEST_CASE(MeterBoss, BossIsTheDamagedEnemyWithTheMostMaxHp) {
    EncounterEngine engine;
    setup_party(engine);
    const Timeline t;
    engine.process_action(hit(kTank, kAdd, 9000, 1), t.at(1));
    engine.process_action(hit(kTank, kBoss, 1000, 2), t.at(2));
    engine.process_enemy_hp(enemy_hp(kAdd, 1000, 10000, 2.2));
    engine.process_enemy_hp(enemy_hp(kBoss, 99000, kBossMaxHp, 2.2));
    // Bigger still, but never damaged this pull.
    engine.process_enemy_hp(enemy_hp(0x40000009, 500000, 500000, 2.2));

    BossSummary boss = engine.current_summary(t.at(3)).boss;
    TEST_ASSERT_EQ(boss.id, kBoss);
    TEST_ASSERT(boss.name == "Titan");
    TEST_ASSERT_NEAR(boss.hp_pct, 99.0, 1e-9);
    TEST_ASSERT(!boss.killed);

    // Equal max HP: the more damaged one.
    engine.process_enemy_hp(enemy_hp(kAdd, 90000, kBossMaxHp, 2.4));
    boss = engine.current_summary(t.at(3)).boss;
    TEST_ASSERT_EQ(boss.id, kAdd);
}

TEST_CASE(MeterBoss, WithoutHpReadsTheBossIsStillNamed) {
    // Vitals off: max HP comes from the actor's registry entry and no HP is known,
    // so the pull reads as it did before the boss was tracked.
    EncounterEngine engine;
    setup_party(engine);
    engine.process_actor_info(actor_info(kAdd, "Granite Gaol", ActorType::Monster, Job::None, 5000, 5000));
    const Timeline t;
    engine.process_action(hit(kTank, kBoss, 1000, 1), t.at(1));
    engine.process_action(hit(kTank, kAdd, 4000, 2), t.at(2));
    engine.update(t.at(10));
    TEST_ASSERT_EQ(engine.state(), EncounterState::Complete);

    const auto pull = engine.latest_pull();
    TEST_ASSERT(pull.has_value());
    TEST_ASSERT_EQ(pull->boss.id, kBoss);
    TEST_ASSERT(pull->boss.name == "Titan");
    TEST_ASSERT(!pull->boss.hp_known());
    TEST_ASSERT(pull_outcome(pull->state, pull->boss) == PullOutcome::Clear);
}

TEST_CASE(MeterBoss, WipeRecordsTheBossHpLeft) {
    EncounterEngine engine;
    setup_party(engine);
    const Timeline t;
    engine.process_action(hit(kTank, kBoss, 1000, 1), t.at(1));
    engine.process_enemy_hp(enemy_hp(kBoss, 60000, kBossMaxHp, 3));
    engine.process_action(hit(kTank, kBoss, 1000, 5), t.at(5));
    engine.process_enemy_hp(enemy_hp(kBoss, 23400, kBossMaxHp, 5.2));
    wipe(engine);
    TEST_ASSERT_EQ(engine.state(), EncounterState::Wipe);

    const auto index = engine.pull_history_index();
    TEST_ASSERT_EQ(index.size(), 1u);
    TEST_ASSERT_EQ(index[0].boss.id, kBoss);
    TEST_ASSERT_NEAR(index[0].boss.hp_pct, 23.4, 1e-9);
    TEST_ASSERT(!index[0].boss.killed);
    TEST_ASSERT(pull_outcome(index[0].state, index[0].boss) == PullOutcome::Wipe);
}

TEST_CASE(MeterBoss, ResetWithTheBossStandingIsEnded) {
    EncounterEngine engine;
    setup_party(engine);
    const Timeline t;
    engine.process_action(hit(kTank, kBoss, 1000, 1), t.at(1));
    engine.process_enemy_hp(enemy_hp(kBoss, 61000, kBossMaxHp, 1.2));
    engine.update(t.at(9));
    TEST_ASSERT_EQ(engine.state(), EncounterState::Complete);

    const auto pull = engine.latest_pull();
    TEST_ASSERT(pull.has_value());
    TEST_ASSERT_NEAR(pull->boss.hp_pct, 61.0, 1e-9);
    TEST_ASSERT(pull_outcome(pull->state, pull->boss) == PullOutcome::Ended);
}

TEST_CASE(MeterBoss, KillIsClearAndStopsTheClockAtTheLastHit) {
    EncounterEngine engine;
    setup_party(engine);
    const Timeline t;
    for (const double s : {1.0, 4.0, 7.0, 10.0}) {
        engine.process_action(hit(kTank, kBoss, 1000, s), t.at(s));
    }
    engine.process_enemy_hp(enemy_hp(kBoss, 50000, kBossMaxHp, 5));
    engine.process_enemy_hp(enemy_hp(kBoss, 0, kBossMaxHp, 10.2));
    // A heal and a HoT tick after the kill keep the pull open, but are not the fight.
    engine.process_action(hit(kHealer, kTank, 500, 14, EffectType::Heal), t.at(14));
    engine.process_status_tick(hot_tick(kHealer, kTank, 300, 17), t.at(17));
    engine.update(t.at(20));
    TEST_ASSERT_EQ(engine.state(), EncounterState::InCombat);
    engine.update(t.at(25));
    TEST_ASSERT_EQ(engine.state(), EncounterState::Complete);

    const auto pull = engine.latest_pull();
    TEST_ASSERT(pull.has_value());
    TEST_ASSERT(pull->boss.killed);
    TEST_ASSERT_NEAR(pull->boss.hp_pct, 0.0, 1e-9);
    TEST_ASSERT_EQ(pull->end_reason, EncounterEndReason::Inactivity);
    TEST_ASSERT(pull_outcome(pull->state, pull->boss) == PullOutcome::Clear);
    TEST_ASSERT_NEAR(pull->duration_seconds, 9.0, 1e-3);
    TEST_ASSERT_EQ(pull->end_time_us, sec(10));
    // Rates follow the trimmed clock.
    TEST_ASSERT_NEAR(pull->total_dps, 4000.0 / 9.0, 1e-3);
}

TEST_CASE(MeterBoss, WithoutAKillTheClockRunsToTheLastActivity) {
    EncounterEngine engine;
    setup_party(engine);
    const Timeline t;
    for (const double s : {1.0, 4.0, 7.0, 10.0}) {
        engine.process_action(hit(kTank, kBoss, 1000, s), t.at(s));
    }
    engine.process_enemy_hp(enemy_hp(kBoss, 40000, kBossMaxHp, 10.2));
    engine.process_status_tick(hot_tick(kHealer, kTank, 300, 14), t.at(14));
    engine.process_status_tick(hot_tick(kHealer, kTank, 300, 17), t.at(17));
    engine.update(t.at(25));

    const auto pull = engine.latest_pull();
    TEST_ASSERT(pull.has_value());
    TEST_ASSERT_NEAR(pull->duration_seconds, 16.0, 1e-3);
}

TEST_CASE(MeterBoss, ManualEndAfterAKillStopsAtTheKill) {
    EncounterEngine engine;
    setup_party(engine);
    const Timeline t;
    engine.process_action(hit(kTank, kBoss, 1000, 1), t.at(1));
    engine.process_action(hit(kTank, kBoss, 1000, 4), t.at(4));
    engine.process_enemy_hp(enemy_hp(kBoss, 0, kBossMaxHp, 4.1));
    engine.end_encounter(EncounterEndReason::Manual, t.at(6), sec(6));

    const auto pull = engine.latest_pull();
    TEST_ASSERT(pull.has_value());
    TEST_ASSERT_NEAR(pull->duration_seconds, 3.0, 1e-3);
    TEST_ASSERT_EQ(pull->end_time_us, sec(4));
}

TEST_CASE(MeterBoss, LateZeroMarksTheArchivedPullKilled) {
    // The wipe can be detected on one lane before the boss's last HP read arrives on
    // the other: a kill read within kLateLifeEventUs of the end still counts.
    EncounterEngine engine;
    setup_party(engine);
    const Timeline t;
    engine.process_action(hit(kTank, kBoss, 1000, 1), t.at(1));
    engine.process_enemy_hp(enemy_hp(kBoss, 300, kBossMaxHp, 4));
    engine.process_action(hit(kTank, kBoss, 1000, 5), t.at(5));
    wipe(engine);
    TEST_ASSERT_EQ(engine.state(), EncounterState::Wipe);
    const uint64_t end_us = engine.latest_pull()->end_time_us;
    const uint64_t revision = engine.history_revision();

    // Another enemy, or a read that is not a kill, changes nothing.
    engine.process_enemy_hp(enemy_hp(kAdd, 0, 5000, 5.5));
    engine.process_enemy_hp(enemy_hp(kBoss, kBossMaxHp, kBossMaxHp, 5.5));  // Reset to full
    TEST_ASSERT(!engine.latest_pull()->boss.killed);
    TEST_ASSERT_NEAR(engine.latest_pull()->boss.hp_pct, 0.3, 1e-9);
    TEST_ASSERT_EQ(engine.history_revision(), revision);

    engine.process_enemy_hp(enemy_hp(kBoss, 0, kBossMaxHp, static_cast<double>(end_us) / 1e6 + 1.0));
    const auto pull = engine.latest_pull();
    TEST_ASSERT(pull->boss.killed);
    // The pull list re-reads the archive on this, so its badge follows the kill.
    TEST_ASSERT_TRUE(engine.history_revision() != revision);
    TEST_ASSERT_NEAR(pull->boss.hp_pct, 0.0, 1e-9);
    TEST_ASSERT(pull_outcome(pull->state, pull->boss) == PullOutcome::Clear);
    TEST_ASSERT(engine.pull_history_index()[0].boss.killed);
    // The live view still shows that pull, so it agrees.
    TEST_ASSERT(engine.current_summary().boss.killed);
}

TEST_CASE(MeterBoss, KillReadTooLateIsDropped) {
    EncounterEngine engine;
    setup_party(engine);
    const Timeline t;
    engine.process_action(hit(kTank, kBoss, 1000, 1), t.at(1));
    engine.process_enemy_hp(enemy_hp(kBoss, 300, kBossMaxHp, 1.2));
    wipe(engine);
    const uint64_t end_us = engine.latest_pull()->end_time_us;
    engine.process_enemy_hp(enemy_hp(kBoss, 0, kBossMaxHp,
        static_cast<double>(end_us + EncounterEngine::kLateLifeEventUs) / 1e6 + 1.0));
    TEST_ASSERT(!engine.latest_pull()->boss.killed);
}

TEST_CASE(MeterBoss, EnemyHpNeverStartsAPullOrKeepsItAlive) {
    EncounterEngine engine;
    setup_party(engine);
    const Timeline t;
    engine.process_enemy_hp(enemy_hp(kBoss, 90000, kBossMaxHp, 0.5));
    TEST_ASSERT_EQ(engine.state(), EncounterState::Idle);

    engine.process_action(hit(kTank, kBoss, 1000, 1), t.at(1));
    for (int s = 2; s <= 12; ++s) {
        engine.process_enemy_hp(enemy_hp(kBoss, 90000 - static_cast<uint32_t>(s), kBossMaxHp, s));
    }
    engine.update(t.at(9));
    TEST_ASSERT_EQ(engine.state(), EncounterState::Complete);
}

TEST_CASE(MeterBoss, NextPullStartsWithoutABoss) {
    EncounterEngine engine;
    setup_party(engine);
    const Timeline t;
    engine.process_action(hit(kTank, kBoss, 1000, 1), t.at(1));
    engine.process_enemy_hp(enemy_hp(kBoss, 0, kBossMaxHp, 1.2));
    engine.update(t.at(10));
    TEST_ASSERT(engine.latest_pull()->boss.killed);

    engine.process_action(hit(kTank, kAdd, 1000, 12), t.at(12));
    const BossSummary boss = engine.current_summary(t.at(13)).boss;
    TEST_ASSERT_EQ(boss.id, kAdd);
    TEST_ASSERT(!boss.hp_known());
    TEST_ASSERT(!boss.killed);
}

// ---------------------------------------------------------------------------
// The payload's vitals pass
// ---------------------------------------------------------------------------

namespace {

std::vector<hub::ipc::EnemyHpPacket> drain_enemy_hp(hub::ipc::PacketRingBuffer& ring, size_t* other = nullptr) {
    std::vector<hub::ipc::EnemyHpPacket> sent;
    std::vector<uint8_t> frame;
    while (ring.pop(frame)) {
        const auto header = hub::ipc::deserialize_header(frame);
        if (!header) continue;
        if (static_cast<hub::MessageType>(header->message_type) != hub::MessageType::CombatEnemyHp) {
            if (other) ++*other;
            continue;
        }
        hub::ipc::EnemyHpPacket hp{};
        std::memcpy(&hp, frame.data() + sizeof(hub::ipc::PacketHeader), sizeof(hp));
        sent.push_back(hp);
    }
    return sent;
}

ActorVitals enemy_vitals(EntityId id, uint32_t hp, uint32_t max_hp) {
    ActorVitals vitals{};
    vitals.entity = id;
    vitals.hp = hp;
    vitals.max_hp = max_hp;
    vitals.is_enemy = true;
    return vitals;
}

} // namespace

TEST_CASE(MeterBoss, PassSendsEnemyHpOnlyWhenItChanged) {
    CombatPlugin plugin;
    plugin.initialize();
    hub::ipc::PacketRingBuffer ring;
    plugin.set_ring_buffer(&ring);
    EncounterEngine& engine = plugin.engine();
    setup_party(engine);
    const Timeline t;
    engine.process_action(hit(kTank, kBoss, 1000, 1), t.at(1));

    std::vector<ActorVitals> pass{enemy_vitals(kBoss, 80000, kBossMaxHp)};
    plugin.on_vitals(pass, sec(1.25));
    auto sent = drain_enemy_hp(ring);
    TEST_ASSERT_EQ(sent.size(), 1u);
    TEST_ASSERT_EQ(sent[0].entity_id, kBoss);
    TEST_ASSERT_EQ(sent[0].current_hp, 80000u);
    TEST_ASSERT_EQ(sent[0].max_hp, kBossMaxHp);
    TEST_ASSERT_EQ(sent[0].timestamp_us, sec(1.25));
    TEST_ASSERT_NEAR(engine.current_summary(t.at(1.3)).boss.hp_pct, 80.0, 1e-9);

    plugin.on_vitals(pass, sec(1.5));
    TEST_ASSERT(drain_enemy_hp(ring).empty());

    pass[0].hp = 70000;
    plugin.on_vitals(pass, sec(1.75));
    TEST_ASSERT_EQ(drain_enemy_hp(ring).size(), 1u);

    // A listener that just connected gets it again.
    plugin.invalidate_published_vitals();
    plugin.on_vitals(pass, sec(2));
    TEST_ASSERT_EQ(drain_enemy_hp(ring).size(), 1u);

    // The pass writes no HP: the registry keeps what actor info said.
    const uint32_t registry_hp = engine.with_registry([](CombatantRegistry& registry) {
        return registry.find_actor(kBoss)->current_hp;
    });
    TEST_ASSERT_EQ(registry_hp, kBossMaxHp);
}

TEST_CASE(MeterBoss, PartyMembersSendNoEnemyHp) {
    CombatPlugin plugin;
    plugin.initialize();
    hub::ipc::PacketRingBuffer ring;
    plugin.set_ring_buffer(&ring);
    setup_party(plugin.engine());

    ActorVitals tank{};
    tank.entity = kTank;
    tank.hp = 50;
    tank.max_hp = 100;
    tank.track_life = true;
    std::vector<ActorVitals> pass{tank, enemy_vitals(kBoss, 0, 0)};  // An unread max proves nothing
    plugin.on_vitals(pass, sec(1));
    TEST_ASSERT(drain_enemy_hp(ring).empty());
}

TEST_CASE(MeterBoss, BothEnginesAgreeOnTheBoss) {
    // The app's engine gets the hits and the HP reads the payload's engine booked.
    CombatPlugin plugin;
    plugin.initialize();
    hub::ipc::PacketRingBuffer ring;
    plugin.set_ring_buffer(&ring);
    EncounterEngine app;
    setup_party(plugin.engine());
    setup_party(app);
    const Timeline t;

    const auto both = [&](const hub::ipc::CombatActionPacket& packet, double s) {
        plugin.engine().process_action(packet, t.at(s));
        app.process_action(packet, t.at(s));
    };
    both(hit(kTank, kBoss, 1000, 1), 1);
    both(hit(kTank, kAdd, 3000, 1.5), 1.5);
    std::vector<ActorVitals> pass{enemy_vitals(kAdd, 100, 5000), enemy_vitals(kBoss, 12000, kBossMaxHp)};
    plugin.on_vitals(pass, sec(2));
    for (const auto& hp : drain_enemy_hp(ring)) app.process_enemy_hp(hp);

    const BossSummary in_game = plugin.engine().current_summary(t.at(3)).boss;
    const BossSummary desktop = app.current_summary(t.at(3)).boss;
    TEST_ASSERT_EQ(in_game.id, kBoss);
    TEST_ASSERT_EQ(desktop.id, in_game.id);
    TEST_ASSERT_NEAR(desktop.hp_pct, in_game.hp_pct, 1e-9);
    TEST_ASSERT_NEAR(desktop.hp_pct, 12.0, 1e-9);
}

TEST_CASE(MeterBoss, WireStructSize) {
    TEST_ASSERT_EQ(sizeof(hub::ipc::CombatEnemyHpPayload), 24u);
}
