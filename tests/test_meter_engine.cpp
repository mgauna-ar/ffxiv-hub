#include "test_framework.hpp"
#include "meter_test_support.hpp"
#include "meter/types.hpp"
#include "meter/combatant_registry.hpp"
#include "meter/encounter_engine.hpp"
#include "meter/combat_plugin.hpp"
#include <atomic>
#include <chrono>
#include <optional>
#include <thread>

using namespace hub::meter;
using namespace hub::test::meter_support;

TEST_CASE(MeterEngine, EncounterLifecycleStartOnAction) {
    EncounterEngine engine;
    TEST_ASSERT_EQ(engine.state(), EncounterState::Idle);

    const auto t0 = std::chrono::steady_clock::now();

    // Passive DoT tick must NOT start combat encounter
    hub::ipc::StatusTickPacket tick{};
    tick.source_id = 100;
    tick.damage_or_heal = 1500;
    tick.effect_type = static_cast<uint8_t>(EffectType::Damage);
    engine.process_status_tick(tick, t0);
    TEST_ASSERT_EQ(engine.state(), EncounterState::Idle);

    // Direct damage action initiates combat encounter
    hub::ipc::CombatActionPacket act{};
    act.source_id = 100;
    act.target_id = 0x40001;
    act.damage = 12000;
    act.action_id = 31;
    act.effect_type = static_cast<uint16_t>(EffectType::Damage);
    engine.process_action(act, t0);

    TEST_ASSERT_EQ(engine.state(), EncounterState::InCombat);
    TEST_ASSERT_TRUE(engine.in_combat());
}

TEST_CASE(MeterEngine, ReaderNowBeforeThePullStartReadsZero) {
    // A reader's default `now` is taken before it locks, so the detour thread can
    // start the pull in between. That `now` must read as no time elapsed, never as a
    // negative duration cast to an unsigned end time.
    EncounterEngine engine;
    const auto t0 = std::chrono::steady_clock::now();

    hub::ipc::CombatActionPacket act{};
    act.source_id = 100;
    act.target_id = 0x40001;
    act.damage = 12000;
    act.action_id = 31;
    act.effect_type = static_cast<uint16_t>(EffectType::Damage);
    engine.process_action(act, t0);
    TEST_ASSERT_TRUE(engine.in_combat());

    const auto before = t0 - std::chrono::seconds(196);
    for (const EncounterSummary& summary : {engine.current_summary(before), engine.current_rankings(before)}) {
        TEST_ASSERT_EQ(summary.duration_seconds, 0.0);
        TEST_ASSERT_EQ(summary.end_time_us, summary.start_time_us);
    }

    engine.update(before);
    TEST_ASSERT_TRUE(engine.in_combat());
    engine.end_encounter(EncounterEndReason::Manual, before);
    const auto history = engine.pull_history();
    TEST_ASSERT_EQ(history.size(), static_cast<size_t>(1));
    TEST_ASSERT_EQ(history.back().duration_seconds, 0.0);
    TEST_ASSERT_EQ(history.back().end_time_us, history.back().start_time_us);
}

TEST_CASE(MeterEngine, InactivityTimeoutSplit) {
    EncounterEngine engine(7.0); // 7.0s timeout
    const auto t0 = std::chrono::steady_clock::now();

    hub::ipc::CombatActionPacket act{};
    act.source_id = 100;
    act.damage = 10000;
    act.effect_type = static_cast<uint16_t>(EffectType::Damage);
    engine.process_action(act, t0);
    TEST_ASSERT_EQ(engine.state(), EncounterState::InCombat);

    // Action at t + 3s
    const auto t1 = t0 + std::chrono::seconds(3);
    act.damage = 15000;
    engine.process_action(act, t1);
    TEST_ASSERT_EQ(engine.state(), EncounterState::InCombat);

    // Advance 7.5s past t1 (t + 10.5s) -> timeout triggers
    const auto t2 = t1 + std::chrono::milliseconds(7500);
    engine.update(t2);

    TEST_ASSERT_EQ(engine.state(), EncounterState::Complete);
    TEST_ASSERT_FALSE(engine.in_combat());

    // Invariant: Duration is calculated from last activity (3s), NOT inflated by 7s timeout
    const auto pull = engine.latest_pull();
    TEST_ASSERT(pull.has_value());
    TEST_ASSERT_NEAR(pull->duration_seconds, 3.0, 0.05);
    TEST_ASSERT_EQ(pull->end_reason, EncounterEndReason::Inactivity);
    TEST_ASSERT_EQ(pull->total_damage, 25000u);
}

namespace {

[[nodiscard]] std::chrono::steady_clock::time_point after(std::chrono::steady_clock::time_point t0, double seconds) {
    return t0 + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds));
}

[[nodiscard]] hub::ipc::CombatActionPacket landed_hit(uint32_t source = 100) {
    hub::ipc::CombatActionPacket act{};
    act.source_id = source;
    act.damage = 10000;
    act.effect_type = static_cast<uint16_t>(EffectType::Damage);
    return act;
}

/// A report and a tick at one moment, the way the payload's loop runs them.
void tick(EncounterEngine& engine, uint32_t client_flags, std::chrono::steady_clock::time_point at) {
    engine.set_game_state(client_flags, at);
    engine.update(at);
}

hub::ipc::CombatActionPacket effect_packet(EffectType effect, uint32_t damage, uint32_t heal) {
    hub::ipc::CombatActionPacket pkt{};
    pkt.source_id = 200;
    pkt.target_id = 100;
    pkt.action_id = 135;
    pkt.damage = damage;
    pkt.effective_heal = heal;
    pkt.effect_type = static_cast<uint16_t>(effect);
    return pkt;
}

} // namespace

TEST_CASE(MeterEngine, GameCombatHoldsAPullThroughDowntime) {
    // A boss's untargetable phase used to split its fight in two after 7 s of quiet.
    EncounterEngine engine;
    const auto t0 = std::chrono::steady_clock::now();

    engine.set_game_state(kInGameCombat, t0);
    engine.process_action(landed_hit(), t0);
    for (int s = 1; s <= 35; ++s) {
        tick(engine, kInGameCombat, after(t0, s));
        if (s == 5 || s == 35) engine.process_action(landed_hit(), after(t0, s));
    }
    TEST_ASSERT_EQ(engine.state(), EncounterState::InCombat);
    TEST_ASSERT(!engine.latest_pull().has_value());

    // The game ends combat at 36; the pull waits out the settle.
    tick(engine, kOutOfGameCombat, after(t0, 36));
    tick(engine, kOutOfGameCombat, after(t0, 37.9));
    TEST_ASSERT_EQ(engine.state(), EncounterState::InCombat);
    tick(engine, kOutOfGameCombat, after(t0, 38.1));
    TEST_ASSERT_EQ(engine.state(), EncounterState::Complete);

    const auto history = engine.pull_history_index();
    TEST_ASSERT_EQ(history.size(), 1u);
    const auto pull = engine.latest_pull();
    TEST_ASSERT_EQ(pull->end_reason, EncounterEndReason::CombatEnded);
    TEST_ASSERT_NEAR(pull->duration_seconds, 35.0, 0.05);
    TEST_ASSERT_EQ(pull->total_damage, 30000u);
}

TEST_CASE(MeterEngine, CombatEndClosesThePullAfterTheSettle) {
    EncounterEngine engine;
    const auto t0 = std::chrono::steady_clock::now();

    engine.set_game_state(kInGameCombat, t0);
    engine.process_action(landed_hit(), t0);
    for (int s = 1; s <= 19; ++s) {
        tick(engine, kInGameCombat, after(t0, s));
        if (s == 5) engine.process_action(landed_hit(), after(t0, s));
    }
    tick(engine, kOutOfGameCombat, after(t0, 20));
    tick(engine, kOutOfGameCombat, after(t0, 21.9));
    TEST_ASSERT_EQ(engine.state(), EncounterState::InCombat);
    tick(engine, kOutOfGameCombat, after(t0, 22.1));
    TEST_ASSERT_EQ(engine.state(), EncounterState::Complete);

    // The quiet after the last hit is not the fight.
    TEST_ASSERT_NEAR(engine.latest_pull()->duration_seconds, 5.0, 0.05);
}

TEST_CASE(MeterEngine, HitsAfterCombatEndsHoldTheClose) {
    // The flag is the local player's alone. If it drops while the party still lands
    // hits, the fight is not over.
    EncounterEngine engine;
    const auto t0 = std::chrono::steady_clock::now();

    engine.set_game_state(kInGameCombat, t0);
    engine.process_action(landed_hit(), t0);
    tick(engine, kOutOfGameCombat, after(t0, 1));
    for (double s : {2.0, 3.5, 5.0}) {
        tick(engine, kOutOfGameCombat, after(t0, s));
        engine.process_action(landed_hit(), after(t0, s));
    }
    tick(engine, kOutOfGameCombat, after(t0, 6.9));
    TEST_ASSERT_EQ(engine.state(), EncounterState::InCombat);
    tick(engine, kOutOfGameCombat, after(t0, 7.1));
    TEST_ASSERT_EQ(engine.state(), EncounterState::Complete);
    TEST_ASSERT_EQ(engine.latest_pull()->total_damage, 40000u);
}

TEST_CASE(MeterEngine, CombatComingBackKeepsOnePull) {
    EncounterEngine engine;
    const auto t0 = std::chrono::steady_clock::now();

    engine.set_game_state(kInGameCombat, t0);
    engine.process_action(landed_hit(), t0);
    tick(engine, kOutOfGameCombat, after(t0, 1));
    tick(engine, kInGameCombat, after(t0, 2));
    engine.process_action(landed_hit(), after(t0, 2.5));
    for (int s = 3; s <= 10; ++s) tick(engine, kInGameCombat, after(t0, s));
    TEST_ASSERT_EQ(engine.state(), EncounterState::InCombat);

    tick(engine, kOutOfGameCombat, after(t0, 11));
    tick(engine, kOutOfGameCombat, after(t0, 13.1));
    TEST_ASSERT_EQ(engine.state(), EncounterState::Complete);
    TEST_ASSERT_EQ(engine.pull_history_index().size(), 1u);
    TEST_ASSERT_EQ(engine.latest_pull()->total_damage, 20000u);
}

TEST_CASE(MeterEngine, WipeThatEndsCombatIsAWipe) {
    // Party HP is read every 1.5 s, so the game can end combat before the wipe is
    // seen. The settle leaves room for it.
    EncounterEngine engine;
    const auto t0 = std::chrono::steady_clock::now();

    hub::ipc::PartySyncPacket sync{};
    sync.party_count = 2;
    sync.entity_ids[0] = 101; sync.job_ids[0] = static_cast<uint32_t>(Job::WAR);
    sync.entity_ids[1] = 102; sync.job_ids[1] = static_cast<uint32_t>(Job::WHM);
    engine.process_party_sync(sync);
    engine.registry_unlocked().update_hp(101, 80000, 80000);
    engine.registry_unlocked().update_hp(102, 60000, 60000);

    engine.set_game_state(kInGameCombat, t0);
    engine.process_action(landed_hit(101), t0);
    tick(engine, kInGameCombat, after(t0, 1));
    tick(engine, kOutOfGameCombat, after(t0, 5));
    TEST_ASSERT_EQ(engine.state(), EncounterState::InCombat);

    engine.registry_unlocked().update_hp(101, 0);
    engine.registry_unlocked().update_hp(102, 0);
    tick(engine, kOutOfGameCombat, after(t0, 6));
    TEST_ASSERT_EQ(engine.state(), EncounterState::Wipe);
    TEST_ASSERT_EQ(engine.latest_pull()->end_reason, EncounterEndReason::Wipe);
}

TEST_CASE(MeterEngine, PullNeverInGameCombatUsesTheIdleTimeout) {
    // Other players fighting nearby: the game never puts us in combat, so nothing
    // says when their fight ended but a quiet stretch.
    EncounterEngine engine;
    const auto t0 = std::chrono::steady_clock::now();

    engine.set_game_state(kOutOfGameCombat, t0);
    engine.process_action(landed_hit(), t0);
    for (int s = 1; s <= 9; ++s) {
        tick(engine, kOutOfGameCombat, after(t0, s));
        if (s == 3) engine.process_action(landed_hit(), after(t0, s));
    }
    tick(engine, kOutOfGameCombat, after(t0, 9.9));
    TEST_ASSERT_EQ(engine.state(), EncounterState::InCombat);
    tick(engine, kOutOfGameCombat, after(t0, 10.1));
    TEST_ASSERT_EQ(engine.state(), EncounterState::Complete);
    TEST_ASSERT_EQ(engine.latest_pull()->end_reason, EncounterEndReason::Inactivity);
    TEST_ASSERT_NEAR(engine.latest_pull()->duration_seconds, 3.0, 0.05);
}

TEST_CASE(MeterEngine, StaleGameStateUsesTheIdleTimeout) {
    // A payload that stops reporting (game closed, pipe dropped) must not hold a pull.
    EncounterEngine engine;
    const auto t0 = std::chrono::steady_clock::now();

    engine.set_game_state(kInGameCombat, t0);
    engine.process_action(landed_hit(), t0);
    engine.process_action(landed_hit(), after(t0, 1));
    TEST_ASSERT(!engine.game_combat(after(t0, 3.1)).has_value());
    engine.update(after(t0, 7.9));
    TEST_ASSERT_EQ(engine.state(), EncounterState::InCombat);
    engine.update(after(t0, 8.1));
    TEST_ASSERT_EQ(engine.state(), EncounterState::Complete);
    TEST_ASSERT_EQ(engine.latest_pull()->end_reason, EncounterEndReason::Inactivity);
}

TEST_CASE(MeterEngine, ReportWithoutValidIsIgnored) {
    // A failed read publishes 0, which says nothing about combat.
    EncounterEngine engine;
    const auto t0 = std::chrono::steady_clock::now();

    engine.set_game_state(kInGameCombat, t0);
    engine.process_action(landed_hit(), t0);
    engine.set_game_state(0, after(t0, 0.5));
    TEST_ASSERT(engine.game_combat(after(t0, 2.9)) == std::optional<bool>(true));
    engine.update(after(t0, 2.9));
    TEST_ASSERT_EQ(engine.state(), EncounterState::InCombat);
}

TEST_CASE(MeterEngine, GameCombatNeverStartsAPull) {
    EncounterEngine engine;
    const auto t0 = std::chrono::steady_clock::now();
    tick(engine, kInGameCombat, t0);
    TEST_ASSERT_EQ(engine.state(), EncounterState::Idle);
    TEST_ASSERT_EQ(engine.pulls_started(), 0u);
}

TEST_CASE(MeterEngine, PartyWipeDetection) {
    EncounterEngine engine;
    const auto t0 = std::chrono::steady_clock::now();

    hub::ipc::PartySyncPacket sync{};
    sync.party_count = 2;
    sync.entity_ids[0] = 101; sync.job_ids[0] = static_cast<uint32_t>(Job::WAR);
    sync.entity_ids[1] = 102; sync.job_ids[1] = static_cast<uint32_t>(Job::WHM);
    engine.process_party_sync(sync);

    engine.registry_unlocked().update_hp(101, 80000, 80000);
    engine.registry_unlocked().update_hp(102, 60000, 60000);

    // Start encounter
    hub::ipc::CombatActionPacket act{};
    act.source_id = 101;
    act.damage = 5000;
    act.effect_type = static_cast<uint16_t>(EffectType::Damage);
    engine.process_action(act, t0);
    TEST_ASSERT_EQ(engine.state(), EncounterState::InCombat);

    // One player dies
    engine.registry_unlocked().update_hp(101, 0);
    engine.update(t0 + std::chrono::seconds(1));
    TEST_ASSERT_EQ(engine.state(), EncounterState::InCombat); // Surviving WHM

    // All dead -> Wipe
    engine.registry_unlocked().update_hp(102, 0);
    engine.update(t0 + std::chrono::seconds(2));
    TEST_ASSERT_EQ(engine.state(), EncounterState::Wipe);

    const auto pull = engine.latest_pull();
    TEST_ASSERT(pull.has_value());
    TEST_ASSERT_EQ(pull->state, EncounterState::Wipe);
    TEST_ASSERT_EQ(pull->end_reason, EncounterEndReason::Wipe);
}

TEST_CASE(MeterEngine, ControlCommandsKeepTheirWireValues) {
    // An older or newer payload sends these as bare bytes.
    using hub::ipc::EncounterControlCommand;
    TEST_ASSERT_EQ(static_cast<int>(EncounterControlCommand::None), 0);
    TEST_ASSERT_EQ(static_cast<int>(EncounterControlCommand::End), 1);
    TEST_ASSERT_EQ(static_cast<int>(EncounterControlCommand::Reset), 2);
    TEST_ASSERT_EQ(static_cast<int>(EncounterControlCommand::Split), 3);

    EncounterEngine engine;
    const auto t0 = std::chrono::steady_clock::now();
    hub::ipc::CombatActionPacket act{};
    act.source_id = 1;
    act.damage = 4200;
    act.effect_type = static_cast<uint16_t>(EffectType::Damage);
    engine.process_action(act, t0);
    TEST_ASSERT_EQ(engine.state(), EncounterState::InCombat);

    // Split archives the running pull and opens the next one straight away.
    hub::ipc::EncounterControlPacket split{};
    split.control_command = EncounterControlCommand::Split;
    engine.process_encounter_control(split, t0 + std::chrono::seconds(5));
    TEST_ASSERT_EQ(engine.state(), EncounterState::InCombat);
    const auto pull = engine.latest_pull();
    TEST_ASSERT(pull.has_value());
    TEST_ASSERT_EQ(pull->end_reason, EncounterEndReason::Manual);

    hub::ipc::EncounterControlPacket end{};
    end.control_command = EncounterControlCommand::End;
    engine.process_encounter_control(end, t0 + std::chrono::seconds(6));
    TEST_ASSERT(engine.state() != EncounterState::InCombat);
}

TEST_CASE(MeterEngine, ConcurrentProducersAndReaders) {
    // The in-game engine is reached by the detour thread, the orchestration
    // thread and the Present thread at once. Run under -fsanitize=thread to
    // catch an entry point that forgot to take the lock.
    EncounterEngine engine;
    // As the app's engine runs, so the timeline is reached from every thread too.
    engine.set_timeline_enabled(true);
    engine.with_registry([](CombatantRegistry& reg) {
        reg.register_actor(1000, "Local Player", Job::WAR, 0, ActorType::Player);
        reg.set_local_player(1000);
    });
    std::atomic<bool> stop{false};

    std::thread producer([&] {
        for (uint32_t i = 0; i < 2000; ++i) {
            hub::ipc::CombatActionPacket pkt{};
            pkt.source_id = 1000 + (i % 8);
            pkt.target_id = 0x40000001;
            pkt.action_id = 31 + (i % 4);
            pkt.damage = 100;
            pkt.effect_type = static_cast<uint16_t>(EffectType::Damage);
            engine.process_action(pkt);

            // The press behind it, from the same detour.
            hub::ipc::CastPacket cast{};
            cast.source_id = 1000 + (i % 8);
            cast.action_id = 31;
            cast.timestamp_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
            engine.process_cast(cast);
        }
        stop.store(true);
    });

    std::thread registrar([&] {
        while (!stop.load()) {
            engine.with_registry([](CombatantRegistry& reg) {
                reg.register_actor(1003, "Party Member", Job::WHM);
            });
        }
    });

    std::thread reader([&] {
        while (!stop.load()) {
            const auto summary = engine.current_summary();
            (void)summary.combatants.size();
            (void)engine.in_combat();
            const auto history = engine.pull_history_index();
            (void)engine.timeline(0).rows.size();
            if (!history.empty()) (void)engine.timeline(history.back().encounter_id).buffs.size();
        }
    });

    std::thread ticker([&] {
        uint32_t n = 0;
        while (!stop.load()) {
            // The game's state rides the same tick, as CombatPlugin::update feeds it.
            engine.set_game_state((++n % 3) ? kInGameCombat : kOutOfGameCombat);
            (void)engine.game_combat();
            engine.update();
        }
    });

    // The orchestration thread's vitals pass.
    std::thread vitals([&] {
        uint64_t ts = 1;
        while (!stop.load()) {
            hub::ipc::StatusListPacket list{};
            list.entity_id = 1003;
            list.timestamp_us = ++ts;
            list.count = static_cast<uint8_t>(ts % 2);
            list.entries[0].status_id = 638;
            list.entries[0].remaining_s = 10.0f;
            engine.process_status_list(list);
            for (const EntityId enemy : engine.tracked_enemies(4)) {
                hub::ipc::EnemyHpPacket hp{};
                hp.entity_id = enemy;
                hp.current_hp = static_cast<uint32_t>(ts % 100);
                hp.max_hp = 100;
                hp.timestamp_us = ts;
                engine.process_enemy_hp(hp);
            }
            const LifeEventKind kind = (ts % 2) ? LifeEventKind::Death : LifeEventKind::Raise;
            engine.process_life_event(engine.build_life_event(1003, kind, ++ts));
        }
    });

    producer.join();
    registrar.join();
    reader.join();
    ticker.join();
    vitals.join();

    TEST_ASSERT(engine.current_summary().total_damage > 0u);
}

TEST_CASE(MeterEngine, WipeDurationTrimsDeadTail) {
    // A wipe is noticed only once the last party member's HP reads zero, which is
    // well after the fight stopped producing damage.
    EncounterEngine engine;
    const auto t0 = std::chrono::steady_clock::now();

    hub::ipc::PartySyncPacket sync{};
    sync.party_count = 1;
    sync.entity_ids[0] = 101;
    sync.job_ids[0] = static_cast<uint32_t>(Job::WAR);
    engine.process_party_sync(sync);
    engine.registry_unlocked().update_hp(101, 80000, 80000);

    hub::ipc::CombatActionPacket act{};
    act.source_id = 101;
    act.damage = 5000;
    act.effect_type = static_cast<uint16_t>(EffectType::Damage);
    engine.process_action(act, t0);
    engine.process_action(act, t0 + std::chrono::seconds(4));
    TEST_ASSERT_EQ(engine.state(), EncounterState::InCombat);

    engine.registry_unlocked().update_hp(101, 0);
    engine.update(t0 + std::chrono::seconds(9));
    TEST_ASSERT_EQ(engine.state(), EncounterState::Wipe);

    const auto pull = engine.latest_pull();
    TEST_ASSERT(pull.has_value());
    TEST_ASSERT_EQ(pull->end_reason, EncounterEndReason::Wipe);
    TEST_ASSERT_NEAR(pull->duration_seconds, 4.0, 0.05);
    TEST_ASSERT_NEAR(pull->total_dps, 2500.0, 1.0);
}

// ---------------------------------------------------------------------------
// What opens a pull. Prepull healing, shielding and buffing is preparation, and
// starting the clock on it meant the fight was already seconds old - with a
// healer's name at the top of a table nobody had hit anything in yet.
// ---------------------------------------------------------------------------

TEST_CASE(MeterEngine, PrepullHealDoesNotStartEncounter) {
    EncounterEngine engine;

    engine.process_action(effect_packet(EffectType::Heal, 0, 8000));
    TEST_ASSERT_FALSE(engine.in_combat());

    // Nothing was banked either: the heal landed before the pull.
    TEST_ASSERT_EQ(engine.accumulator_unlocked().total_healing(), 0u);

    // The first real hit is what opens it.
    engine.process_action(effect_packet(EffectType::Damage, 25000, 0));
    TEST_ASSERT_TRUE(engine.in_combat());
    TEST_ASSERT_EQ(engine.accumulator_unlocked().total_damage(), 25000u);
}

TEST_CASE(MeterEngine, OnlyLandedDamageOpensAnEncounter) {
    // Buffs, debuffs and whiffs are not the start of a fight either.
    for (const auto& pkt : {
             effect_packet(EffectType::Heal, 0, 8000),
             effect_packet(EffectType::Buff, 0, 0),
             effect_packet(EffectType::Debuff, 0, 0),
             effect_packet(EffectType::Miss, 0, 0),
             effect_packet(EffectType::Damage, 0, 0),
         }) {
        EncounterEngine engine;
        engine.process_action(pkt);
        TEST_ASSERT_FALSE(engine.in_combat());
    }

    // A blocked or parried hit is mitigated, not avoided, so it still counts.
    for (const auto& pkt : {
             effect_packet(EffectType::Damage, 100, 0),
             effect_packet(EffectType::Blocked, 100, 0),
             effect_packet(EffectType::Parried, 100, 0),
         }) {
        EncounterEngine engine;
        engine.process_action(pkt);
        TEST_ASSERT_TRUE(engine.in_combat());
    }
}

TEST_CASE(MeterEngine, HealingStillCountsOnceTheFightIsUnderway) {
    // The start rule must not make healers invisible mid-fight.
    EncounterEngine engine;

    engine.process_action(effect_packet(EffectType::Damage, 25000, 0));
    TEST_ASSERT_TRUE(engine.in_combat());

    engine.process_action(effect_packet(EffectType::Heal, 0, 8000));
    TEST_ASSERT_EQ(engine.accumulator_unlocked().total_healing(), 8000u);
    TEST_ASSERT_TRUE(engine.in_combat());
}

TEST_CASE(MeterEngine, DeathsArrivingAsActorInfoEndThePullAsAWipe) {
    // The app's engine only ever learns HP from ActorInfo packets.
    EncounterEngine engine;
    engine.process_actor_info(party_actor(1001, Job::WAR, 80000, 80000));
    engine.process_actor_info(party_actor(1002, Job::WHM, 60000, 60000));

    hub::ipc::PartySyncPacket party{};
    party.party_count = 2;
    party.entity_ids[0] = 1001; party.job_ids[0] = static_cast<uint32_t>(Job::WAR);
    party.entity_ids[1] = 1002; party.job_ids[1] = static_cast<uint32_t>(Job::WHM);
    engine.process_party_sync(party);

    auto hit = effect_packet(EffectType::Damage, 25000, 0);
    hit.source_id = 1001;
    hit.target_id = 0x40000001;
    engine.process_action(hit);
    TEST_ASSERT_TRUE(engine.in_combat());

    engine.process_actor_info(party_actor(1001, Job::WAR, 0, 80000));
    TEST_ASSERT_TRUE(engine.in_combat());
    engine.process_actor_info(party_actor(1002, Job::WHM, 0, 60000));
    TEST_ASSERT_FALSE(engine.in_combat());

    const auto pull = engine.latest_pull();
    TEST_ASSERT_TRUE(pull.has_value());
    TEST_ASSERT(pull->end_reason == EncounterEndReason::Wipe);
}

TEST_CASE(MeterEngine, SoloDeathEndsThePullAsAWipe) {
    // The app's engine learns the local player from the party sync and its death
    // from actor info, the same as a party member's.
    EncounterEngine engine;
    hub::ipc::PartySyncPacket solo{};
    solo.local_player_id = 1001;
    engine.process_party_sync(solo);
    engine.process_actor_info(party_actor(1001, Job::WAR, 80000, 80000));
    engine.process_actor_info(party_actor(1002, Job::WHM, 60000, 60000));

    auto hit = effect_packet(EffectType::Damage, 25000, 0);
    hit.source_id = 1001;
    hit.target_id = 0x40000001;
    engine.process_action(hit);
    TEST_ASSERT_TRUE(engine.in_combat());

    engine.process_actor_info(party_actor(1001, Job::WAR, 0, 80000));
    TEST_ASSERT_FALSE(engine.in_combat());
    const auto pull = engine.latest_pull();
    TEST_ASSERT_TRUE(pull.has_value());
    TEST_ASSERT(pull->end_reason == EncounterEndReason::Wipe);
}
