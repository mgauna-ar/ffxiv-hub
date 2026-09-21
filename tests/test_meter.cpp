#include "test_framework.hpp"
#include "meter/types.hpp"
#include "hub/game/entity.hpp"
#include "hub/game/pets.hpp"
#include "meter/action_decoder.hpp"
#include "meter/combatant_registry.hpp"
#include "meter/metrics_accumulator.hpp"
#include "meter/encounter_engine.hpp"
#include "meter/pull_grouping.hpp"
#include "meter/combat_plugin.hpp"
#include "common/config/json.hpp"
#include <algorithm>
#include <array>
#include <vector>
#include <atomic>
#include <thread>
#include <cstring>

using namespace hub::meter;

TEST_CASE(MeterDecoder, SingleTargetDamageAndHitSeverity) {
    hub::game::ActionEffectHeader header{};
    header.animation_target_id = 0x40001234;
    header.action_id = 31; // Heavy Swing
    header.num_targets = 1;

    std::array<hub::game::ActionEffectEntry, 8> entries{};
    // Normal hit
    entries[0].effect_type = 0x03; // Damage
    entries[0].hit_severity = 0x00; // Normal
    entries[0].value = 12500;

    auto packets = decoder::decode_action_effects(1001, header, entries.data(), nullptr, 999999);
    TEST_ASSERT_EQ(packets.size(), 1u);
    TEST_ASSERT_EQ(packets[0].source_id, 1001u);
    TEST_ASSERT_EQ(packets[0].target_id, 0x40001234u);
    TEST_ASSERT_EQ(packets[0].action_id, 31u);
    TEST_ASSERT_EQ(packets[0].damage, 12500u);
    TEST_ASSERT_EQ(packets[0].effective_heal, 0u);
    TEST_ASSERT_EQ(packets[0].effect_type, static_cast<uint16_t>(EffectType::Damage));
    TEST_ASSERT_EQ(packets[0].severity, static_cast<uint8_t>(HitSeverity::Normal));
    TEST_ASSERT_EQ(packets[0].timestamp_us, 999999u);

    // Critical Hit (tested with both FFXIV client bit 0x20 and mock 0x01)
    entries[0].hit_severity = 0x20; // Real FFXIV client critical flag
    packets = decoder::decode_action_effects(1001, header, entries.data(), nullptr);
    TEST_ASSERT_EQ(packets[0].severity, static_cast<uint8_t>(HitSeverity::Critical));
    TEST_ASSERT((packets[0].hit_flags & HitFlags::Crit) != 0);

    entries[0].hit_severity = 0x01; // Mock flag fallback
    packets = decoder::decode_action_effects(1001, header, entries.data(), nullptr);
    TEST_ASSERT_EQ(packets[0].severity, static_cast<uint8_t>(HitSeverity::Critical));

    // Direct Hit (tested with both FFXIV client bit 0x40 and mock 0x02)
    entries[0].hit_severity = 0x40; // Real FFXIV client direct hit flag
    packets = decoder::decode_action_effects(1001, header, entries.data(), nullptr);
    TEST_ASSERT_EQ(packets[0].severity, static_cast<uint8_t>(HitSeverity::DirectHit));
    TEST_ASSERT((packets[0].hit_flags & HitFlags::DirectHit) != 0);

    entries[0].hit_severity = 0x02; // Mock flag fallback
    packets = decoder::decode_action_effects(1001, header, entries.data(), nullptr);
    TEST_ASSERT_EQ(packets[0].severity, static_cast<uint8_t>(HitSeverity::DirectHit));

    // Crit Direct Hit (0x20 | 0x40 = 0x60)
    entries[0].hit_severity = 0x60; // Real FFXIV client CDH flag
    packets = decoder::decode_action_effects(1001, header, entries.data(), nullptr);
    TEST_ASSERT_EQ(packets[0].severity, static_cast<uint8_t>(HitSeverity::CritDirectHit));
    TEST_ASSERT((packets[0].hit_flags & HitFlags::Crit) != 0);
    TEST_ASSERT((packets[0].hit_flags & HitFlags::DirectHit) != 0);

    entries[0].hit_severity = 0x03; // Mock flag fallback
    packets = decoder::decode_action_effects(1001, header, entries.data(), nullptr);
    TEST_ASSERT_EQ(packets[0].severity, static_cast<uint8_t>(HitSeverity::CritDirectHit));

    // Extended 24-bit Dawntrail damage (e.g. 150,000 damage: value=18928, high_byte=2, flags=0x40)
    entries[0].value = static_cast<uint16_t>(150000 & 0xFFFF);
    entries[0].high_byte = static_cast<uint8_t>((150000 >> 16) & 0xFF);
    entries[0].flags = 0x40;
    entries[0].hit_severity = 0x60;
    packets = decoder::decode_action_effects(1001, header, entries.data(), nullptr);
    TEST_ASSERT_EQ(packets[0].damage, 150000u);
    TEST_ASSERT_EQ(packets[0].severity, static_cast<uint8_t>(HitSeverity::CritDirectHit));
}

TEST_CASE(MeterDecoder, HealingAndMissAndBlocked) {
    hub::game::ActionEffectHeader header{};
    header.animation_target_id = 0x1000;
    header.action_id = 120; // Cure
    header.num_targets = 1;

    std::array<hub::game::ActionEffectEntry, 8> entries{};
    // Heal
    entries[0].effect_type = 0x04;
    entries[0].value = 18000;
    entries[0].hit_severity = 0x20; // Crit heal (0x20)

    auto packets = decoder::decode_action_effects(2001, header, entries.data(), nullptr);
    TEST_ASSERT_EQ(packets.size(), 1u);
    TEST_ASSERT_EQ(packets[0].effective_heal, 18000u);
    TEST_ASSERT_EQ(packets[0].damage, 0u);
    TEST_ASSERT_EQ(packets[0].effect_type, static_cast<uint16_t>(EffectType::Heal));
    TEST_ASSERT_EQ(packets[0].severity, static_cast<uint8_t>(HitSeverity::Critical));

    // Extended 24-bit heal (85,000 heal: value = 19464, high_byte = 1, flags = 0x40)
    entries[0].value = static_cast<uint16_t>(85000 & 0xFFFF);
    entries[0].high_byte = static_cast<uint8_t>((85000 >> 16) & 0xFF);
    entries[0].flags = 0x40;
    packets = decoder::decode_action_effects(2001, header, entries.data(), nullptr);
    TEST_ASSERT_EQ(packets[0].effective_heal, 85000u);

    // Miss
    entries[0] = {};
    entries[0].effect_type = 0x01;
    entries[0].value = 0;
    entries[0].hit_severity = 0;
    packets = decoder::decode_action_effects(2001, header, entries.data(), nullptr);
    TEST_ASSERT_EQ(packets.size(), 1u);
    TEST_ASSERT_EQ(packets[0].effect_type, static_cast<uint16_t>(EffectType::Miss));
    TEST_ASSERT((packets[0].hit_flags & HitFlags::Miss) != 0);

    // Blocked
    entries[0] = {};
    entries[0].effect_type = 0x05;
    entries[0].value = 5000;
    packets = decoder::decode_action_effects(2001, header, entries.data(), nullptr);
    TEST_ASSERT_EQ(packets.size(), 1u);
    TEST_ASSERT_EQ(packets[0].effect_type, static_cast<uint16_t>(EffectType::Blocked));
    TEST_ASSERT_EQ(packets[0].damage, 5000u);
    TEST_ASSERT((packets[0].hit_flags & HitFlags::Blocked) != 0);

    // Parried
    entries[0] = {};
    entries[0].effect_type = 0x06;
    entries[0].value = 6200;
    packets = decoder::decode_action_effects(2001, header, entries.data(), nullptr);
    TEST_ASSERT_EQ(packets.size(), 1u);
    TEST_ASSERT_EQ(packets[0].effect_type, static_cast<uint16_t>(EffectType::Parried));
    TEST_ASSERT_EQ(packets[0].damage, 6200u);
    TEST_ASSERT((packets[0].hit_flags & HitFlags::Parried) != 0);
}

TEST_CASE(MeterDecoder, MultiTargetAoEDistribution) {
    hub::game::ActionEffectHeader header{};
    header.animation_target_id = 0x40000001;
    header.action_id = 3571; // Assize
    header.num_targets = 3;

    uint64_t targets[3] = { 0x40000001, 0x40000002, 0x40000003 };

    std::array<hub::game::ActionEffectEntry, 24> entries{};
    // Target 0: Damage 20000 + Heal 15000
    entries[0].effect_type = 0x03;
    entries[0].value = 20000;
    entries[1].effect_type = 0x04;
    entries[1].value = 15000;

    // Target 1: Damage 21000
    entries[8].effect_type = 0x03;
    entries[8].value = 21000;

    // Target 2: Damage 19500
    entries[16].effect_type = 0x03;
    entries[16].value = 19500;

    auto packets = decoder::decode_action_effects(1001, header, entries.data(), targets);
    TEST_ASSERT_EQ(packets.size(), 4u);

    TEST_ASSERT_EQ(packets[0].target_id, 0x40000001u);
    TEST_ASSERT_EQ(packets[0].damage, 20000u);

    TEST_ASSERT_EQ(packets[1].target_id, 0x40000001u);
    TEST_ASSERT_EQ(packets[1].effective_heal, 15000u);

    TEST_ASSERT_EQ(packets[2].target_id, 0x40000002u);
    TEST_ASSERT_EQ(packets[2].damage, 21000u);

    TEST_ASSERT_EQ(packets[3].target_id, 0x40000003u);
    TEST_ASSERT_EQ(packets[3].damage, 19500u);
}

TEST_CASE(MeterRegistry, ActorRegistrationAndPetInference) {
    CombatantRegistry reg;

    reg.register_actor(100, "Warrior Tank", Job::WAR, 0, ActorType::Player, 120000, 120000);
    const auto* war = reg.find_actor(100);
    TEST_ASSERT(war != nullptr);
    TEST_ASSERT_EQ(war->name, "Warrior Tank");
    TEST_ASSERT_EQ(war->job, Job::WAR);
    TEST_ASSERT_EQ(war->role, Role::Tank);
    TEST_ASSERT_FALSE(war->is_pet);

    // Pet name job inference
    reg.register_actor(201, "Demi-Bahamut");
    const auto* baha = reg.find_actor(201);
    TEST_ASSERT(baha != nullptr);
    TEST_ASSERT(baha->is_pet);
    TEST_ASSERT_EQ(baha->job, Job::SMN);

    reg.register_actor(202, "Eos");
    const auto* eos = reg.find_actor(202);
    TEST_ASSERT(eos != nullptr);
    TEST_ASSERT(eos->is_pet);
    TEST_ASSERT_EQ(eos->job, Job::SCH);

    reg.register_actor(203, "Automaton Queen");
    const auto* queen = reg.find_actor(203);
    TEST_ASSERT(queen != nullptr);
    TEST_ASSERT(queen->is_pet);
    TEST_ASSERT_EQ(queen->job, Job::MCH);

    reg.register_actor(204, "Living Shadow");
    const auto* shadow = reg.find_actor(204);
    TEST_ASSERT(shadow != nullptr);
    TEST_ASSERT(shadow->is_pet);
    TEST_ASSERT_EQ(shadow->job, Job::DRK);

    reg.register_actor(205, "Bunshin");
    const auto* bunshin = reg.find_actor(205);
    TEST_ASSERT(bunshin != nullptr);
    TEST_ASSERT(bunshin->is_pet);
    TEST_ASSERT_EQ(bunshin->job, Job::NIN);
}

TEST_CASE(MeterRegistry, PetOwnerResolutionAndLoopGuard) {
    CombatantRegistry reg;

    reg.register_actor(10, "Summoner Player", Job::SMN);
    reg.register_actor(20, "Demi-Bahamut", Job::SMN, 10);

    TEST_ASSERT_EQ(reg.resolve_owner(10), 10u);
    TEST_ASSERT_EQ(reg.resolve_owner(20), 10u);

    // Chained ownership: 30 -> 20 -> 10
    reg.set_pet_owner(30, 20);
    TEST_ASSERT_EQ(reg.resolve_owner(30), 10u);

    // Cyclic loop guard (A -> B -> A)
    reg.set_pet_owner(100, 101);
    reg.set_pet_owner(101, 100);
    // Should terminate gracefully without stack overflow or infinite loop
    const EntityId resolved = reg.resolve_owner(100);
    TEST_ASSERT(resolved == 100u || resolved == 101u);
}

TEST_CASE(MeterRegistry, PartySyncAndWipeDetection) {
    CombatantRegistry reg;

    hub::ipc::PartySyncPacket pkt{};
    pkt.party_count = 3;
    pkt.entity_ids[0] = 1001; pkt.job_ids[0] = static_cast<uint32_t>(Job::WAR);
    pkt.entity_ids[1] = 1002; pkt.job_ids[1] = static_cast<uint32_t>(Job::WHM);
    pkt.entity_ids[2] = 1003; pkt.job_ids[2] = static_cast<uint32_t>(Job::VPR);

    reg.sync_party(pkt);
    TEST_ASSERT_EQ(reg.party_size(), 3u);
    TEST_ASSERT_TRUE(reg.is_party_member(1001));
    TEST_ASSERT_TRUE(reg.is_party_member(1002));
    TEST_ASSERT_TRUE(reg.is_party_member(1003));
    TEST_ASSERT_FALSE(reg.is_party_member(9999));

    // Initially HP is default, not wiped
    reg.update_hp(1001, 80000, 80000);
    reg.update_hp(1002, 60000, 60000);
    reg.update_hp(1003, 70000, 70000);
    TEST_ASSERT_FALSE(reg.is_party_wiped());

    // 2 members die, 1 survivor: NOT wiped
    reg.update_hp(1001, 0);
    reg.update_hp(1002, 0);
    TEST_ASSERT_FALSE(reg.is_party_wiped());

    // Last member dies: Wiped
    reg.update_hp(1003, 0);
    TEST_ASSERT_TRUE(reg.is_party_wiped());

    // Healer gets revived: Wipe condition clears immediately
    reg.update_hp(1002, 30000);
    TEST_ASSERT_FALSE(reg.is_party_wiped());
}

TEST_CASE(MeterRegistry, FriendlyFiltering) {
    CombatantRegistry reg;

    reg.set_local_player(500);
    TEST_ASSERT_TRUE(reg.is_friendly(500));

    reg.register_actor(600, "Boss Monster", Job::None, 0, ActorType::Monster);
    TEST_ASSERT_FALSE(reg.is_friendly(600));

    // Entity with monster bit 0x40000000
    TEST_ASSERT_FALSE(reg.is_friendly(0x40000123));
}

TEST_CASE(MeterAccumulator, DamageAccumulationAndDps) {
    MetricsAccumulator acc;
    CombatantRegistry reg;
    reg.register_actor(100, "Viper", Job::VPR);

    hub::ipc::CombatActionPacket p1{};
    p1.source_id = 100;
    p1.target_id = 0x40001;
    p1.action_id = 34606; // Steel Fangs
    p1.damage = 10000;
    p1.effect_type = static_cast<uint16_t>(EffectType::Damage);
    p1.severity = static_cast<uint8_t>(HitSeverity::Normal);
    acc.record_action(p1, reg);

    hub::ipc::CombatActionPacket p2{};
    p2.source_id = 100;
    p2.target_id = 0x40001;
    p2.action_id = 34607; // Reaving Fangs
    p2.damage = 20000;
    p2.effect_type = static_cast<uint16_t>(EffectType::Damage);
    p2.severity = static_cast<uint8_t>(HitSeverity::CritDirectHit);
    p2.hit_flags = HitFlags::Crit | HitFlags::DirectHit;
    acc.record_action(p2, reg);

    TEST_ASSERT_EQ(acc.total_damage(), 30000u);

    // Duration floor test: duration = 0.0s clamped to 1.0s
    acc.recalculate(0.0, &reg);
    TEST_ASSERT_NEAR(acc.total_dps(), 30000.0, 0.01);

    // Duration = 10.0s -> DPS = 3000.0
    acc.recalculate(10.0, &reg);
    TEST_ASSERT_NEAR(acc.total_dps(), 3000.0, 0.01);

    const auto* stats = acc.find_stats(100);
    TEST_ASSERT(stats != nullptr);
    TEST_ASSERT_EQ(stats->total_damage, 30000u);
    TEST_ASSERT_NEAR(stats->dps, 3000.0, 0.01);
    TEST_ASSERT_NEAR(stats->hits.crit_rate(), 50.0, 0.01);
    TEST_ASSERT_NEAR(stats->hits.dh_rate(), 50.0, 0.01);
    TEST_ASSERT_NEAR(stats->hits.cdh_rate(), 50.0, 0.01);
}

TEST_CASE(MeterAccumulator, HealingAndOverhealAccounting) {
    MetricsAccumulator acc;
    CombatantRegistry reg;
    reg.register_actor(200, "White Mage", Job::WHM);

    hub::ipc::CombatActionPacket p1{};
    p1.source_id = 200;
    p1.target_id = 100;
    p1.action_id = 135; // Cure II
    p1.damage = 10000; // total raw heal
    p1.effective_heal = 7000;
    p1.overheal = 3000;
    p1.effect_type = static_cast<uint16_t>(EffectType::Heal);
    acc.record_action(p1, reg);

    TEST_ASSERT_EQ(acc.total_healing(), 10000u);
    TEST_ASSERT_EQ(acc.total_effective_healing(), 7000u);
    TEST_ASSERT_EQ(acc.total_overhealing(), 3000u);
    TEST_ASSERT_NEAR(acc.overheal_pct(), 30.0, 0.01);

    // HPS calculation is strictly effective healing / duration
    acc.recalculate(10.0, &reg);
    TEST_ASSERT_NEAR(acc.total_hps(), 700.0, 0.01);

    const auto* whm = acc.find_stats(200);
    TEST_ASSERT(whm != nullptr);
    TEST_ASSERT_NEAR(whm->hps, 700.0, 0.01);
    TEST_ASSERT_NEAR(whm->overheal_pct(), 30.0, 0.01);
}

TEST_CASE(MeterAccumulator, PetDamageAttributionAndLateMerge) {
    MetricsAccumulator acc;
    CombatantRegistry reg;

    reg.register_actor(10, "Summoner", Job::SMN);
    reg.register_actor(20, "Demi-Bahamut", Job::SMN, 10);

    // Summoner action
    hub::ipc::CombatActionPacket p1{};
    p1.source_id = 10;
    p1.damage = 15000;
    p1.effect_type = static_cast<uint16_t>(EffectType::Damage);
    p1.action_id = 25820; // Astral Impulse
    acc.record_action(p1, reg);

    // Pet action
    hub::ipc::CombatActionPacket p2{};
    p2.source_id = 20; // Bahamut
    p2.damage = 25000;
    p2.effect_type = static_cast<uint16_t>(EffectType::Damage);
    p2.action_id = 7449; // Akh Morn
    acc.record_action(p2, reg);

    // Both should be attributed to Summoner (id 10)
    const auto* smn = acc.find_stats(10);
    TEST_ASSERT(smn != nullptr);
    TEST_ASSERT_EQ(smn->total_damage, 40000u);
    TEST_ASSERT_EQ(smn->pet_damage, 25000u);
    // Pet should NOT have an orphan row
    TEST_ASSERT(acc.find_stats(20) == nullptr);

    // Test late pet merge: an unlinked pet recorded first, then merged
    hub::ipc::CombatActionPacket p3{};
    p3.source_id = 99; // Unlinked pet
    p3.damage = 5000;
    p3.effect_type = static_cast<uint16_t>(EffectType::Damage);
    p3.action_id = 100;
    acc.record_action(p3, reg);
    TEST_ASSERT(acc.find_stats(99) != nullptr);

    // Late attribution: link pet 99 to owner 10
    reg.set_pet_owner(99, 10);
    acc.recalculate(5.0, &reg);

    // Row 99 merged into 10
    TEST_ASSERT(acc.find_stats(99) == nullptr);
    smn = acc.find_stats(10);
    TEST_ASSERT_EQ(smn->total_damage, 45000u);
    TEST_ASSERT_EQ(smn->pet_damage, 30000u);
}

TEST_CASE(MeterAccumulator, FriendlyDamageIsolation) {
    MetricsAccumulator acc;
    CombatantRegistry reg;

    reg.register_actor(10, "Warrior", Job::WAR);
    reg.register_actor(0x400001, "Raid Boss", Job::None, 0, ActorType::Monster);

    // Boss deals 50,000 damage to Warrior
    hub::ipc::CombatActionPacket p{};
    p.source_id = 0x400001;
    p.target_id = 10;
    p.damage = 50000;
    p.effect_type = static_cast<uint16_t>(EffectType::Damage);
    acc.record_action(p, reg);

    // Invariant: Boss damage must NEVER pollute raid damage or raid DPS
    TEST_ASSERT_EQ(acc.total_damage(), 0u);
    acc.recalculate(10.0, &reg);
    TEST_ASSERT_NEAR(acc.total_dps(), 0.0, 0.01);

    // But Warrior's damage_taken is tracked
    const auto* war = acc.find_stats(10);
    TEST_ASSERT(war != nullptr);
    TEST_ASSERT_EQ(war->damage_taken, 50000u);
}

TEST_CASE(MeterAccumulator, SortingByDpsAndHps) {
    MetricsAccumulator acc;
    CombatantRegistry reg;

    reg.register_actor(1, "Dps 1", Job::VPR);
    reg.register_actor(2, "Dps 2", Job::PCT);
    reg.register_actor(3, "Healer", Job::WHM);

    hub::ipc::CombatActionPacket p1{}; p1.source_id = 1; p1.damage = 10000; p1.effect_type = 1; acc.record_action(p1, reg);
    hub::ipc::CombatActionPacket p2{}; p2.source_id = 2; p2.damage = 30000; p2.effect_type = 1; acc.record_action(p2, reg);
    hub::ipc::CombatActionPacket p3{}; p3.source_id = 3; p3.damage = 5000;  p3.effect_type = 1; acc.record_action(p3, reg);

    hub::ipc::CombatActionPacket h1{}; h1.source_id = 3; h1.effective_heal = 20000; h1.effect_type = 2; acc.record_action(h1, reg);

    acc.recalculate(10.0, &reg);

    auto dps_list = acc.sorted_by_dps();
    TEST_ASSERT_EQ(dps_list.size(), 3u);
    TEST_ASSERT_EQ(dps_list[0].entity_id, 2u); // 30,000
    TEST_ASSERT_EQ(dps_list[1].entity_id, 1u); // 10,000
    TEST_ASSERT_EQ(dps_list[2].entity_id, 3u); // 5,000

    auto hps_list = acc.sorted_by_hps();
    TEST_ASSERT_FALSE(hps_list.empty());
    TEST_ASSERT_EQ(hps_list[0].entity_id, 3u); // 20,000 heal
}

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

TEST_CASE(MeterEngine, PartyWipeDetection) {
    EncounterEngine engine;
    const auto t0 = std::chrono::steady_clock::now();

    hub::ipc::PartySyncPacket sync{};
    sync.party_count = 2;
    sync.entity_ids[0] = 101; sync.job_ids[0] = static_cast<uint32_t>(Job::WAR);
    sync.entity_ids[1] = 102; sync.job_ids[1] = static_cast<uint32_t>(Job::WHM);
    engine.process_party_sync(sync);

    engine.registry().update_hp(101, 80000, 80000);
    engine.registry().update_hp(102, 60000, 60000);

    // Start encounter
    hub::ipc::CombatActionPacket act{};
    act.source_id = 101;
    act.damage = 5000;
    act.effect_type = static_cast<uint16_t>(EffectType::Damage);
    engine.process_action(act, t0);
    TEST_ASSERT_EQ(engine.state(), EncounterState::InCombat);

    // One player dies
    engine.registry().update_hp(101, 0);
    engine.update(t0 + std::chrono::seconds(1));
    TEST_ASSERT_EQ(engine.state(), EncounterState::InCombat); // Surviving WHM

    // All dead -> Wipe
    engine.registry().update_hp(102, 0);
    engine.update(t0 + std::chrono::seconds(2));
    TEST_ASSERT_EQ(engine.state(), EncounterState::Wipe);

    const auto pull = engine.latest_pull();
    TEST_ASSERT(pull.has_value());
    TEST_ASSERT_EQ(pull->state, EncounterState::Wipe);
    TEST_ASSERT_EQ(pull->end_reason, EncounterEndReason::Wipe);
}

TEST_CASE(MeterEngine, ZoneChangeArchivesPullAndTagsSummary) {
    EncounterEngine engine;
    const auto t0 = std::chrono::steady_clock::now();

    // A control packet only announcing a zone must not start an encounter.
    hub::ipc::EncounterControlPacket zone{};
    zone.zone_id = 1000;
    engine.process_encounter_control(zone, t0);
    TEST_ASSERT_EQ(engine.current_zone_id(), 1000u);
    TEST_ASSERT_EQ(engine.state(), EncounterState::Idle);

    hub::ipc::CombatActionPacket act{};
    act.source_id = 1;
    act.damage = 4200;
    act.effect_type = static_cast<uint16_t>(EffectType::Damage);
    engine.process_action(act, t0);
    TEST_ASSERT_EQ(engine.state(), EncounterState::InCombat);

    zone.zone_id = 1001;
    engine.process_encounter_control(zone, t0 + std::chrono::seconds(3));
    TEST_ASSERT_EQ(engine.current_zone_id(), 1001u);

    const auto pull = engine.latest_pull();
    TEST_ASSERT(pull.has_value());
    TEST_ASSERT_EQ(pull->end_reason, EncounterEndReason::ZoneChange);
    // The pull is filed under the zone it was fought in, not the new one.
    TEST_ASSERT_EQ(pull->zone_id, 1000u);
}

TEST_CASE(MeterEngine, ZoneLabelPrefersNameThenTableThenId) {
    // An explicitly supplied name always wins.
    TEST_ASSERT(zone_label(1238, "The Omega Protocol") == "The Omega Protocol");
    // Then the generated territory table, which is why the payload never has to
    // resolve a zone name itself.
    TEST_ASSERT(zone_label(1238, "") == "Futures Rewritten (Ultimate)");
    // Then the raw id, for a territory the sheet carries no duty for.
    TEST_ASSERT(zone_label(999999, "") == "Zone #999999");
    TEST_ASSERT(zone_label(0, "").empty());
}

TEST_CASE(MeterEngine, TerritoryTableIsSortedAndResolves) {
    // territory_name binary-searches the table, so a bad regeneration that left it
    // unsorted would fail lookups silently.
    TEST_ASSERT_TRUE(std::is_sorted(
        hub::game::TERRITORY_TABLE.begin(), hub::game::TERRITORY_TABLE.end(),
        [](const auto& a, const auto& b) { return a.id < b.id; }));
    TEST_ASSERT(hub::game::territory_name(1238) == "Futures Rewritten (Ultimate)");
    TEST_ASSERT(hub::game::territory_name(1000) == "the Excitatron 6000");
    TEST_ASSERT(hub::game::territory_name(999999).empty());
}

TEST_CASE(MeterEngine, PullHistoryArchive) {
    EncounterEngine engine;
    engine.set_history_capacity(3);

    for (int i = 1; i <= 5; ++i) {
        const auto now = std::chrono::steady_clock::now();
        hub::ipc::CombatActionPacket act{};
        act.source_id = 1;
        act.damage = static_cast<uint32_t>(i * 1000);
        act.effect_type = static_cast<uint16_t>(EffectType::Damage);
        engine.process_action(act, now);
        engine.end_encounter(EncounterEndReason::Manual, now + std::chrono::seconds(2));
    }

    // Capacity is capped at 3
    TEST_ASSERT_EQ(engine.pull_history().size(), 3u);
    TEST_ASSERT_EQ(engine.pull_history()[2].total_damage, 5000u);
    TEST_ASSERT_EQ(engine.pull_history()[1].total_damage, 4000u);
    TEST_ASSERT_EQ(engine.pull_history()[0].total_damage, 3000u);

    // Archived pulls carry a wall-clock end so the app can show when they happened.
    TEST_ASSERT(engine.pull_history()[2].ended_at_unix_s > 0u);
}

TEST_CASE(MeterPlugin, PluginLifecycleAndConfig) {
    CombatPlugin plugin;
    TEST_ASSERT_EQ(plugin.id(), hub::PluginId::CombatMeter);
    TEST_ASSERT_EQ(std::string(plugin.name()), "Combat Meter");

    TEST_ASSERT_TRUE(plugin.initialize());

    // Serialize default config to JSON
    hub::config::JsonValue json{hub::config::JsonValue::ObjectType{}};
    plugin.serialize_config(json);
    TEST_ASSERT_TRUE(json["enabled"].as_bool(false));
    TEST_ASSERT_NEAR(json["inactivity_timeout_seconds"].as_double(0.0), 7.0, 0.01);
    TEST_ASSERT_TRUE(json["party_only"].as_bool(false));

    // Modify and deserialize back
    json["inactivity_timeout_seconds"] = hub::config::JsonValue(10.0);
    json["party_only"] = hub::config::JsonValue(false);
    json["overlay_width"] = hub::config::JsonValue(950);
    plugin.deserialize_config(json);

    TEST_ASSERT_NEAR(plugin.config().inactivity_timeout_seconds, 10.0, 0.01);
    TEST_ASSERT_NEAR(plugin.engine().inactivity_timeout(), 10.0, 0.01);
    TEST_ASSERT_FALSE(plugin.config().party_only);
    TEST_ASSERT_NEAR(plugin.config().overlay.width, 950.0f, 0.01f);

    plugin.shutdown();
}

TEST_CASE(MeterPlugin, HookConsumerDispatch) {
    CombatPlugin plugin;
    plugin.initialize();

    hub::game::ActionEffectHeader header{};
    header.animation_target_id = 0x40001;
    header.action_id = 31;
    header.num_targets = 1;

    std::array<hub::game::ActionEffectEntry, 8> entries{};
    entries[0].effect_type = 0x03; // Damage
    entries[0].value = 25000;

    hub::game::CharacterObject chr{};
    chr.entity_id = 777;
    std::string test_name = "Krile";
    std::copy(test_name.begin(), test_name.end(), chr.name);
    chr.class_job = static_cast<uint8_t>(Job::PCT);
    chr.object_kind = 1; // Player
    chr.owner_id = 0xE0000000; // Game's "no owner" sentinel
    chr.current_hp = 50000;
    chr.max_hp = 50000;

    plugin.on_receive_action_effect(
        777,
        &chr,
        &header,
        entries.data(),
        nullptr
    );

    TEST_ASSERT_TRUE(plugin.engine().in_combat());
    TEST_ASSERT_EQ(plugin.engine().accumulator().total_damage(), 25000u);

    const auto* actor = plugin.engine().registry().find_actor(777);
    TEST_ASSERT(actor != nullptr);
    TEST_ASSERT_EQ(actor->name, "Krile");
    TEST_ASSERT_EQ(actor->job, Job::PCT);
    TEST_ASSERT_EQ(actor->actor_type, ActorType::Player);
    TEST_ASSERT_EQ(actor->owner_id, 0u);
    TEST_ASSERT_TRUE(plugin.engine().registry().is_friendly(777));

    plugin.shutdown();
}

TEST_CASE(MeterPlugin, CountsDamageOverTimeTicks) {
    // ProcessHotDot ticks were dropped entirely, so every DoT/HoT was missing
    // from the totals.
    CombatPlugin plugin;
    plugin.initialize();

    hub::ipc::PacketRingBuffer ring;
    plugin.set_ring_buffer(&ring);

    // Open an encounter so ticks have somewhere to land.
    hub::game::ActionEffectHeader header{};
    header.animation_target_id = 0x40001;
    header.action_id = 31;
    header.num_targets = 1;

    std::array<hub::game::ActionEffectEntry, 8> entries{};
    entries[0].effect_type = 0x03;
    entries[0].value = 1000;

    hub::game::CharacterObject chr{};
    chr.entity_id = 777;
    chr.object_kind = 1;
    chr.owner_id = 0xE0000000;
    chr.current_hp = 50000;
    chr.max_hp = 50000;

    plugin.on_receive_action_effect(777, &chr, &header, entries.data(), nullptr);
    TEST_ASSERT_TRUE(plugin.engine().in_combat());

    const uint64_t damage_before = plugin.engine().accumulator().total_damage();

    // damage_type != 0 and tick_mode != 4 => damage-over-time
    plugin.on_status_tick(0x40000123, 777, 1871, 4500, /*is_heal=*/false);

    TEST_ASSERT_EQ(plugin.engine().accumulator().total_damage(), damage_before + 4500u);

    const uint64_t healing_before = plugin.engine().accumulator().total_healing();
    plugin.on_status_tick(777, 777, 158, 2200, /*is_heal=*/true);
    TEST_ASSERT_EQ(plugin.engine().accumulator().total_healing(), healing_before + 2200u);

    plugin.shutdown();
}

TEST_CASE(MeterPlugin, StatusTickIgnoredWhenDisabledOrTargetless) {
    CombatPlugin plugin;
    plugin.initialize();
    plugin.config().enabled = false;

    plugin.on_status_tick(0x40000123, 777, 1871, 4500, false);
    TEST_ASSERT_FALSE(plugin.engine().in_combat());

    plugin.config().enabled = true;
    plugin.on_status_tick(0, 777, 1871, 4500, false);
    TEST_ASSERT_FALSE(plugin.engine().in_combat());

    plugin.shutdown();
}

TEST_CASE(MeterPlugin, MapsGameObjectKindToActorType) {
    CombatPlugin plugin;
    plugin.initialize();

    hub::game::ActionEffectHeader header{};
    header.animation_target_id = 0x40001;
    header.action_id = 31;
    header.num_targets = 1;

    std::array<hub::game::ActionEffectEntry, 8> entries{};
    entries[0].effect_type = 0x03;
    entries[0].value = 1000;

    // object_kind 2 is a monster in the game, but 2 is ActorType::Pet - a direct
    // cast would file every enemy as a friendly pet.
    hub::game::CharacterObject monster{};
    monster.entity_id = 0x40000123;
    monster.object_kind = 2;
    monster.owner_id = 0xE0000000;
    monster.current_hp = 9000;
    monster.max_hp = 9000;

    plugin.on_receive_action_effect(0x40000123, &monster, &header, entries.data(), nullptr);

    const auto* enemy = plugin.engine().registry().find_actor(0x40000123);
    TEST_ASSERT(enemy != nullptr);
    TEST_ASSERT_EQ(enemy->actor_type, ActorType::Monster);
    TEST_ASSERT_FALSE(plugin.engine().registry().is_friendly(0x40000123));

    // object_kind 5 is a pet, which ActorType spells 2.
    hub::game::CharacterObject pet{};
    pet.entity_id = 888;
    pet.object_kind = 5;
    pet.owner_id = 777;
    pet.current_hp = 100;
    pet.max_hp = 100;

    plugin.on_receive_action_effect(888, &pet, &header, entries.data(), nullptr);

    const auto* pet_actor = plugin.engine().registry().find_actor(888);
    TEST_ASSERT(pet_actor != nullptr);
    TEST_ASSERT_EQ(pet_actor->actor_type, ActorType::Pet);
    TEST_ASSERT_EQ(pet_actor->owner_id, 777u);

    plugin.shutdown();
}

TEST_CASE(MeterPlugin, EmitsCombatActionOverIpc) {
    CombatPlugin plugin;
    plugin.initialize();

    hub::ipc::PacketRingBuffer ring;
    plugin.set_ring_buffer(&ring);

    hub::game::ActionEffectHeader header{};
    header.animation_target_id = 0x40001;
    header.action_id = 31;
    header.num_targets = 1;

    std::array<hub::game::ActionEffectEntry, 8> entries{};
    entries[0].effect_type = 0x03; // Damage
    entries[0].value = 25000;

    plugin.on_receive_action_effect(777, nullptr, &header, entries.data(), nullptr);

    std::vector<uint8_t> item;
    TEST_ASSERT_TRUE(ring.pop(item));

    auto pkt_header = hub::ipc::deserialize_header(item);
    TEST_ASSERT_TRUE(pkt_header.has_value());
    TEST_ASSERT(pkt_header->plugin_id == static_cast<uint16_t>(hub::PluginId::CombatMeter));
    TEST_ASSERT(pkt_header->message_type == static_cast<uint16_t>(hub::MessageType::CombatAction));

    hub::ipc::CombatActionPayload payload{};
    std::memcpy(&payload, item.data() + sizeof(hub::ipc::PacketHeader), sizeof(payload));
    TEST_ASSERT_EQ(payload.action_id, 31u);
    TEST_ASSERT_EQ(payload.damage, 25000u);
}

TEST_CASE(MeterEngine, ConcurrentProducersAndReaders) {
    // The in-game engine is reached by the detour thread, the orchestration
    // thread and the Present thread at once. Run under -fsanitize=thread to
    // catch an entry point that forgot to take the lock.
    EncounterEngine engine;
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
            (void)engine.pull_history_index();
        }
    });

    std::thread ticker([&] {
        while (!stop.load()) {
            engine.update();
        }
    });

    producer.join();
    registrar.join();
    reader.join();
    ticker.join();

    TEST_ASSERT(engine.current_summary().total_damage > 0u);
}

TEST_CASE(MeterAccumulator, LimitBreakIsNotPersonalDamage) {
    // Limit Break belongs to the party, so it must not inflate the DPS of whoever
    // happened to press it.
    MetricsAccumulator acc;
    CombatantRegistry reg;

    reg.register_actor(10, "Monk", Job::MNK, 0, ActorType::Player);
    reg.set_local_player(10);

    hub::ipc::CombatActionPacket normal{};
    normal.source_id = 10;
    normal.target_id = 0x400001;
    normal.action_id = 31;
    normal.damage = 20000;
    normal.effect_type = static_cast<uint16_t>(EffectType::Damage);
    acc.record_action(normal, reg);

    hub::ipc::CombatActionPacket lb = normal;
    lb.action_id = 200; // Braver
    lb.damage = 180000;
    acc.record_action(lb, reg);

    acc.recalculate(10.0, &reg);

    const auto* monk = acc.find_stats(10);
    TEST_ASSERT(monk != nullptr);
    TEST_ASSERT_EQ(monk->total_damage, 20000u);
    TEST_ASSERT_NEAR(monk->dps, 2000.0, 0.01);

    const auto* lb_row = acc.find_stats(hub::game::LIMIT_BREAK_COMBATANT_ID);
    TEST_ASSERT(lb_row != nullptr);
    TEST_ASSERT(lb_row->name == "Limit Break");
    TEST_ASSERT_EQ(lb_row->actor_type, ActorType::LimitBreak);
    TEST_ASSERT_EQ(lb_row->total_damage, 180000u);
    TEST_ASSERT_FALSE(lb_row->is_pet);

    // Raid totals still account for every point of damage dealt.
    TEST_ASSERT_EQ(acc.total_damage(), 200000u);
    TEST_ASSERT_NEAR(acc.total_dps(), 20000.0, 0.01);

    // And the boss took all of it.
    const auto* boss = acc.find_stats(0x400001);
    TEST_ASSERT(boss != nullptr);
    TEST_ASSERT_EQ(boss->damage_taken, 200000u);
}

TEST_CASE(MeterAccumulator, LimitBreakRowSurvivesTheMonsterFilter) {
    // The synthetic id sits above every real entity id, which means it also has the
    // monster bit set - without a special case the row would be filtered as an enemy.
    MetricsAccumulator acc;
    CombatantRegistry reg;

    reg.register_actor(10, "Dragoon", Job::DRG, 0, ActorType::Player);

    hub::ipc::CombatActionPacket lb{};
    lb.source_id = 10;
    lb.target_id = 0x400001;
    lb.action_id = 4242; // Dragonsong Dive
    lb.damage = 150000;
    lb.effect_type = static_cast<uint16_t>(EffectType::Damage);
    acc.record_action(lb, reg);
    acc.recalculate(10.0, &reg);

    TEST_ASSERT_TRUE(reg.is_friendly(hub::game::LIMIT_BREAK_COMBATANT_ID));

    const auto* lb_row = acc.find_stats(hub::game::LIMIT_BREAK_COMBATANT_ID);
    TEST_ASSERT(lb_row != nullptr);
    TEST_ASSERT_EQ(lb_row->actor_type, ActorType::LimitBreak);
    TEST_ASSERT_TRUE(lb_row->is_friendly());
    TEST_ASSERT_EQ(acc.total_damage(), 150000u);
    TEST_ASSERT_NEAR(lb_row->damage_share_pct, 100.0, 0.01);

    // It survives the party-only filter that hides monsters, and the caster keeps a
    // row of their own with none of the damage.
    const auto rows = acc.sorted_by_dps(/*friendly_only=*/true);
    TEST_ASSERT_EQ(rows.size(), 1u);
    TEST_ASSERT(rows.front().name == "Limit Break");
}

TEST_CASE(MeterAccumulator, BlockedAndParriedDamageCounts) {
    // Blocked and parried hits are mitigated, not nullified: the value that got
    // through is damage dealt and belongs in the totals.
    MetricsAccumulator acc;
    CombatantRegistry reg;
    reg.register_actor(10, "Samurai", Job::SAM, 0, ActorType::Player);

    hub::ipc::CombatActionPacket p{};
    p.source_id = 10;
    p.target_id = 0x400001;
    p.action_id = 31;
    p.damage = 9000;
    p.effect_type = static_cast<uint16_t>(EffectType::Damage);
    acc.record_action(p, reg);

    p.damage = 4000;
    p.effect_type = static_cast<uint16_t>(EffectType::Blocked);
    acc.record_action(p, reg);

    p.damage = 3000;
    p.effect_type = static_cast<uint16_t>(EffectType::Parried);
    acc.record_action(p, reg);

    const auto* sam = acc.find_stats(10);
    TEST_ASSERT(sam != nullptr);
    TEST_ASSERT_EQ(sam->total_damage, 16000u);
    TEST_ASSERT_EQ(acc.total_damage(), 16000u);
    TEST_ASSERT_EQ(sam->hits.blocked_hits, 1u);
    TEST_ASSERT_EQ(sam->hits.parried_hits, 1u);
    TEST_ASSERT_EQ(sam->hits.total_hits, 3u);

    const auto act = sam->actions.find(31);
    TEST_ASSERT(act != sam->actions.end());
    TEST_ASSERT_EQ(act->second.total_damage, 16000u);
    TEST_ASSERT_EQ(act->second.damage_hits, 3u);
    TEST_ASSERT_EQ(act->second.hits.blocked_hits, 1u);

    const auto* boss = acc.find_stats(0x400001);
    TEST_ASSERT(boss != nullptr);
    TEST_ASSERT_EQ(boss->damage_taken, 16000u);
}

TEST_CASE(MeterAccumulator, CritRateExcludesNonDamageHits) {
    // Misses and heals have no severity, so counting them in the denominator
    // understated every crit rate.
    MetricsAccumulator acc;
    CombatantRegistry reg;
    reg.register_actor(10, "Sage", Job::SGE, 0, ActorType::Player);

    hub::ipc::CombatActionPacket crit{};
    crit.source_id = 10;
    crit.target_id = 0x400001;
    crit.action_id = 24283;
    crit.damage = 10000;
    crit.severity = static_cast<uint8_t>(HitSeverity::Critical);
    crit.effect_type = static_cast<uint16_t>(EffectType::Damage);
    acc.record_action(crit, reg);

    hub::ipc::CombatActionPacket normal = crit;
    normal.damage = 5000;
    normal.severity = static_cast<uint8_t>(HitSeverity::Normal);
    acc.record_action(normal, reg);

    hub::ipc::CombatActionPacket miss = normal;
    miss.damage = 0;
    miss.effect_type = static_cast<uint16_t>(EffectType::Miss);
    acc.record_action(miss, reg);

    hub::ipc::CombatActionPacket heal{};
    heal.source_id = 10;
    heal.target_id = 10;
    heal.action_id = 24284;
    heal.effective_heal = 8000;
    heal.effect_type = static_cast<uint16_t>(EffectType::Heal);
    acc.record_action(heal, reg);

    const auto* sge = acc.find_stats(10);
    TEST_ASSERT(sge != nullptr);
    TEST_ASSERT_EQ(sge->hits.rated_hits(), 2u);
    TEST_ASSERT_NEAR(sge->hits.crit_rate(), 50.0, 0.01);
    // Heals are counted, just not against the damage rates.
    TEST_ASSERT_EQ(sge->heal_hit_counts.total_hits, 1u);
    TEST_ASSERT_EQ(sge->effective_healing, 8000u);
}

TEST_CASE(MeterAccumulator, DotTicksDoNotDiluteCritRate) {
    // ProcessHotDot reports no crit flag, so a tick must not be booked as a
    // guaranteed non-crit.
    MetricsAccumulator acc;
    CombatantRegistry reg;
    reg.register_actor(10, "Black Mage", Job::BLM, 0, ActorType::Player);

    hub::ipc::CombatActionPacket crit{};
    crit.source_id = 10;
    crit.target_id = 0x400001;
    crit.action_id = 16505;
    crit.damage = 30000;
    crit.severity = static_cast<uint8_t>(HitSeverity::Critical);
    crit.effect_type = static_cast<uint16_t>(EffectType::Damage);
    acc.record_action(crit, reg);

    hub::ipc::StatusTickPacket tick{};
    tick.source_id = 10;
    tick.target_id = 0x400001;
    tick.status_id = 1871;
    tick.damage_or_heal = 4500;
    tick.effect_type = static_cast<uint8_t>(EffectType::Damage);
    acc.record_status_tick(tick, reg);
    acc.record_status_tick(tick, reg);

    const auto* blm = acc.find_stats(10);
    TEST_ASSERT(blm != nullptr);
    TEST_ASSERT_EQ(blm->total_damage, 39000u);
    TEST_ASSERT_EQ(blm->hits.tick_hits, 2u);
    TEST_ASSERT_EQ(blm->hits.rated_hits(), 1u);
    TEST_ASSERT_NEAR(blm->hits.crit_rate(), 100.0, 0.01);
}

TEST_CASE(MeterAccumulator, StatusIdDoesNotCollideWithActionId) {
    // Statuses and actions are separate id spaces, so sharing one map merged a DoT
    // into an unrelated ability's row.
    MetricsAccumulator acc;
    CombatantRegistry reg;
    reg.register_actor(10, "Ninja", Job::NIN, 0, ActorType::Player);

    constexpr uint32_t kSharedId = 1205;

    hub::ipc::CombatActionPacket p{};
    p.source_id = 10;
    p.target_id = 0x400001;
    p.action_id = kSharedId;
    p.damage = 7000;
    p.effect_type = static_cast<uint16_t>(EffectType::Damage);
    acc.record_action(p, reg);

    hub::ipc::StatusTickPacket tick{};
    tick.source_id = 10;
    tick.target_id = 0x400001;
    tick.status_id = static_cast<uint16_t>(kSharedId);
    tick.damage_or_heal = 2000;
    tick.effect_type = static_cast<uint8_t>(EffectType::Damage);
    acc.record_status_tick(tick, reg);

    const auto* nin = acc.find_stats(10);
    TEST_ASSERT(nin != nullptr);
    TEST_ASSERT_EQ(nin->actions.size(), 2u);

    const auto action_row = nin->actions.find(kSharedId);
    TEST_ASSERT(action_row != nin->actions.end());
    TEST_ASSERT_EQ(action_row->second.total_damage, 7000u);
    TEST_ASSERT_FALSE(action_row->second.is_status);

    const auto status_row = nin->actions.find(kSharedId | STATUS_ACTION_KEY_OFFSET);
    TEST_ASSERT(status_row != nin->actions.end());
    TEST_ASSERT_EQ(status_row->second.total_damage, 2000u);
    TEST_ASSERT_TRUE(status_row->second.is_status);
    TEST_ASSERT_EQ(status_row->second.action_id, kSharedId);
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
    engine.registry().update_hp(101, 80000, 80000);

    hub::ipc::CombatActionPacket act{};
    act.source_id = 101;
    act.damage = 5000;
    act.effect_type = static_cast<uint16_t>(EffectType::Damage);
    engine.process_action(act, t0);
    engine.process_action(act, t0 + std::chrono::seconds(4));
    TEST_ASSERT_EQ(engine.state(), EncounterState::InCombat);

    engine.registry().update_hp(101, 0);
    engine.update(t0 + std::chrono::seconds(9));
    TEST_ASSERT_EQ(engine.state(), EncounterState::Wipe);

    const auto pull = engine.latest_pull();
    TEST_ASSERT(pull.has_value());
    TEST_ASSERT_EQ(pull->end_reason, EncounterEndReason::Wipe);
    TEST_ASSERT_NEAR(pull->duration_seconds, 4.0, 0.05);
    TEST_ASSERT_NEAR(pull->total_dps, 2500.0, 1.0);
}

TEST_CASE(MeterAccumulator, EnemyLimitBreakStaysAnEnemyHit) {
    // ActionCategory 9 also holds duty-action and NPC limit breaks, so an action id
    // match alone must not hand a boss's cast to the party's Limit Break row.
    MetricsAccumulator acc;
    CombatantRegistry reg;

    reg.register_actor(10, "Dancer", Job::DNC, 0, ActorType::Player);
    reg.register_actor(0x400001, "Raid Boss", Job::None, 0, ActorType::Monster);

    hub::ipc::CombatActionPacket p{};
    p.source_id = 0x400001;
    p.target_id = 10;
    p.action_id = 29936; // Diamond Dust, a duty-action limit break.
    p.damage = 60000;
    p.effect_type = static_cast<uint16_t>(EffectType::Damage);
    acc.record_action(p, reg);

    TEST_ASSERT(acc.find_stats(hub::game::LIMIT_BREAK_COMBATANT_ID) == nullptr);
    TEST_ASSERT_EQ(acc.total_damage(), 0u);

    const auto* dnc = acc.find_stats(10);
    TEST_ASSERT(dnc != nullptr);
    TEST_ASSERT_EQ(dnc->damage_taken, 60000u);
}

TEST_CASE(MeterLimitBreak, ActionTableCoversEveryTier) {
    // Spot checks against the game's ActionCategory 9 sheet.
    TEST_ASSERT_TRUE(hub::game::is_limit_break_action(197));   // Shield Wall
    TEST_ASSERT_TRUE(hub::game::is_limit_break_action(200));   // Braver
    TEST_ASSERT_TRUE(hub::game::is_limit_break_action(208));   // Pulse of Life
    TEST_ASSERT_TRUE(hub::game::is_limit_break_action(4242));  // Dragonsong Dive
    TEST_ASSERT_TRUE(hub::game::is_limit_break_action(24858)); // The End
    TEST_ASSERT_TRUE(hub::game::is_limit_break_action(34867)); // Chromatic Fantasy
    TEST_ASSERT_TRUE(hub::game::is_limit_break_action(44288)); // Mercy's Justice

    TEST_ASSERT_FALSE(hub::game::is_limit_break_action(0));
    TEST_ASSERT_FALSE(hub::game::is_limit_break_action(31));    // Heavy Swing
    TEST_ASSERT_FALSE(hub::game::is_limit_break_action(196));
    TEST_ASSERT_FALSE(hub::game::is_limit_break_action(209));
    TEST_ASSERT_FALSE(hub::game::is_limit_break_action(16505)); // Despair

    // The table must stay sorted: is_limit_break_action binary-searches it.
    TEST_ASSERT_TRUE(std::is_sorted(hub::game::LIMIT_BREAK_ACTIONS.begin(),
                                    hub::game::LIMIT_BREAK_ACTIONS.end()));
}

TEST_CASE(MeterAccumulator, PlaceholderEntityIdOpensNoRow) {
    // 0xE0000000 is the game's placeholder, not an actor: it fills the target slot of
    // an effect that hit nothing (891 of them in one Zeromus EX clear) and the owner
    // slot of an ownerless actor. It is never a Limit Break source.
    MetricsAccumulator acc;
    CombatantRegistry reg;
    reg.register_actor(10, "Reaper", Job::RPR, 0, ActorType::Player);

    // Placeholder source: not an actor, so nothing is recorded.
    hub::ipc::CombatActionPacket from_placeholder{};
    from_placeholder.source_id = hub::game::NO_ENTITY_ID;
    from_placeholder.target_id = 0x400001;
    from_placeholder.action_id = 31;
    from_placeholder.damage = 1000;
    from_placeholder.effect_type = static_cast<uint16_t>(EffectType::Damage);
    acc.record_action(from_placeholder, reg);

    TEST_ASSERT(acc.find_stats(hub::game::LIMIT_BREAK_COMBATANT_ID) == nullptr);
    TEST_ASSERT(acc.find_stats(hub::game::NO_ENTITY_ID) == nullptr);
    TEST_ASSERT_EQ(acc.total_damage(), 0u);

    // Placeholder target: the hit still counts for the caster, but opens no row.
    hub::ipc::CombatActionPacket at_nothing{};
    at_nothing.source_id = 10;
    at_nothing.target_id = hub::game::NO_ENTITY_ID;
    at_nothing.action_id = 24858; // the End
    at_nothing.damage = 0;
    at_nothing.effect_type = static_cast<uint16_t>(EffectType::Damage);
    acc.record_action(at_nothing, reg);

    TEST_ASSERT(acc.find_stats(hub::game::NO_ENTITY_ID) == nullptr);
}

TEST_CASE(MeterGameData, BeastmasterResolvesToAJob) {
    // job.hpp stopped at PCT, so a BST in the party read as Job::None with no role,
    // which also made CombatantRegistry::is_friendly's role check miss.
    TEST_ASSERT_EQ(static_cast<uint32_t>(Job::BST), 43u);
    TEST_ASSERT(hub::game::to_string(Job::BST) == "Beastmaster");
    TEST_ASSERT(hub::game::job_abbreviation(Job::BST) == "BST");
    TEST_ASSERT_EQ(hub::game::job_to_role(Job::BST), Role::Melee);

    CombatantRegistry reg;
    reg.register_actor(10, "Tamer Mcgee", Job::BST);
    TEST_ASSERT_TRUE(reg.is_friendly(10));
    const auto* bst = reg.find_actor(10);
    TEST_ASSERT(bst != nullptr);
    TEST_ASSERT_EQ(bst->role, Role::Melee);
}

TEST_CASE(MeterGameData, ActionAndStatusNamesComeFromTheSheets) {
    // The hand-written table held 42 actions, so the drilldown showed "Action 36954"
    // for nearly everything, and every DoT row read "Status <id>".
    TEST_ASSERT(hub::game::action_name(200) == "Braver");
    TEST_ASSERT(hub::game::action_name(31) == "Heavy Swing");
    TEST_ASSERT(hub::game::action_name(36954) == "Lance Barrage");
    TEST_ASSERT(hub::game::status_name(1871) == "Dia");
    // Ids the sheets do not carry still degrade to the readable fallback.
    TEST_ASSERT(hub::game::action_name(999999) == "Action 999999");
    TEST_ASSERT(hub::game::status_name(999999) == "Status 999999");
}

TEST_CASE(MeterRegistry, PrimalBossIsNotAPet) {
    // Pet matching was a substring test, so "Titan" matched the pet name "titan":
    // the boss was flagged as a pet and dropped from the meter entirely.
    CombatantRegistry reg;
    MetricsAccumulator acc;
    reg.register_actor(10, "Warrior", Job::WAR, 0, ActorType::Player);
    reg.register_actor(0x400001, "Titan", Job::None, 0, ActorType::Monster);

    const auto* boss = reg.find_actor(0x400001);
    TEST_ASSERT(boss != nullptr);
    TEST_ASSERT_FALSE(boss->is_pet);
    TEST_ASSERT_EQ(boss->owner_id, 0u);

    hub::ipc::CombatActionPacket p{};
    p.source_id = 0x400001;
    p.target_id = 10;
    p.damage = 50000;
    p.effect_type = static_cast<uint16_t>(EffectType::Damage);
    acc.record_action(p, reg);
    acc.recalculate(10.0, &reg);

    const auto rows = acc.sorted_by_dps(/*friendly_only=*/false);
    TEST_ASSERT_EQ(rows.size(), 2u);
    TEST_ASSERT_TRUE(std::any_of(rows.begin(), rows.end(),
                                 [](const CombatantStats& c) { return c.name == "Titan"; }));
}

TEST_CASE(MeterRegistry, PlayerNamedShadowIsNotAPet) {
    // Same substring bug from the other direction: an ordinary player name that
    // happens to contain a pet name had its damage merged into someone else's row.
    for (const char* name : {"Shadowdove", "Theos", "Queenie", "Titania Fae"}) {
        TEST_ASSERT_FALSE(hub::game::is_known_pet_name(name));
        TEST_ASSERT_EQ(hub::game::infer_pet_job(name), Job::None);
    }
    // Real pets still resolve, case-insensitively, and still name their owner job.
    TEST_ASSERT_TRUE(hub::game::is_known_pet_name("Eos"));
    TEST_ASSERT_TRUE(hub::game::is_known_pet_name("living shadow"));
    TEST_ASSERT_EQ(hub::game::infer_pet_job("Demi-Bahamut"), Job::SMN);
    TEST_ASSERT_EQ(hub::game::infer_pet_job("Automaton Queen"), Job::MCH);
    TEST_ASSERT_EQ(hub::game::infer_pet_job("Bunshin"), Job::NIN);
}

TEST_CASE(MeterPullGrouping, GroupsZonesNewestFirst) {
    // Archive order is chronological, so the last entry is the newest pull.
    std::vector<PullHistoryEntry> history;
    for (uint32_t zone : {1238u, 1u, 1238u, 1u, 1238u}) {
        PullHistoryEntry entry{};
        entry.encounter_id = history.size() + 1;
        entry.zone_id = zone;
        history.push_back(entry);
    }

    const auto groups = group_pulls_by_zone(history);
    TEST_ASSERT_EQ(groups.size(), 2u);

    // 1238 owns the newest pull (index 4), so it leads.
    TEST_ASSERT_EQ(groups[0].zone_id, 1238u);
    TEST_ASSERT_EQ(groups[0].pulls, (std::vector<size_t>{4, 2, 0}));
    TEST_ASSERT_EQ(groups[1].zone_id, 1u);
    TEST_ASSERT_EQ(groups[1].pulls, (std::vector<size_t>{3, 1}));
}

TEST_CASE(MeterPullGrouping, RevisitedZoneFoldsIntoOneGroup) {
    // Leaving a duty and coming back must not open a second group for it.
    std::vector<PullHistoryEntry> history(4);
    history[0].zone_id = 1238;
    history[1].zone_id = 1;
    history[2].zone_id = 1;
    history[3].zone_id = 1238;

    const auto groups = group_pulls_by_zone(history);
    TEST_ASSERT_EQ(groups.size(), 2u);
    TEST_ASSERT_EQ(groups[0].pulls.size(), 2u);
    TEST_ASSERT_EQ(groups[1].pulls.size(), 2u);
}

TEST_CASE(MeterPullGrouping, EveryGroupCarriesALabel) {
    std::vector<PullHistoryEntry> history(3);
    history[0].zone_id = 0;               // never entered a zone
    history[1].zone_id = 1238;            // resolved from the territory table
    history[2].zone_id = 999999;          // not in the sheet
    history[2].zone_name = "Somewhere";   // explicit name wins

    const auto groups = group_pulls_by_zone(history);
    TEST_ASSERT_EQ(groups.size(), 3u);
    for (const auto& group : groups) {
        TEST_ASSERT_FALSE(group.label.empty());
    }
    TEST_ASSERT_EQ(groups[0].label, std::string("Somewhere"));
    TEST_ASSERT_EQ(groups[1].label, std::string("Futures Rewritten (Ultimate)"));
    TEST_ASSERT_EQ(groups[2].label, std::string("Unknown zone"));
}

TEST_CASE(MeterEngine, EncounterIdsSurviveEviction) {
    // The picker keys its selection on encounter_id precisely because the archive
    // evicts from the front: if ids were renumbered, the selection would drift onto
    // a different pull exactly like a positional index does.
    EncounterEngine engine;
    engine.set_history_capacity(2);

    auto t = std::chrono::steady_clock::now();
    for (int i = 0; i < 3; ++i) {
        engine.start_encounter(t);
        engine.end_encounter(EncounterEndReason::Manual, t + std::chrono::seconds(10));
        t += std::chrono::seconds(30);
    }

    const auto index = engine.pull_history_index();
    TEST_ASSERT_EQ(index.size(), 2u);
    TEST_ASSERT_EQ(index[0].encounter_id, 2u);
    TEST_ASSERT_EQ(index[1].encounter_id, 3u);
}
