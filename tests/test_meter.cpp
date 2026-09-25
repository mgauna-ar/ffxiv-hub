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
#include "payload/object_reader.hpp"
#include <algorithm>
#include <array>
#include <vector>
#include <atomic>
#include <chrono>
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

    // Critical Hit
    entries[0].hit_severity = 0x20; // Real FFXIV client critical flag
    packets = decoder::decode_action_effects(1001, header, entries.data(), nullptr);
    TEST_ASSERT_EQ(packets[0].severity, static_cast<uint8_t>(HitSeverity::Critical));
    TEST_ASSERT((packets[0].hit_flags & HitFlags::Crit) != 0);

    // Only the game's bits count: the low bits are neither a crit nor a direct hit.
    entries[0].hit_severity = 0x01;
    packets = decoder::decode_action_effects(1001, header, entries.data(), nullptr);
    TEST_ASSERT_EQ(packets[0].severity, static_cast<uint8_t>(HitSeverity::Normal));

    // Direct Hit
    entries[0].hit_severity = 0x40; // Real FFXIV client direct hit flag
    packets = decoder::decode_action_effects(1001, header, entries.data(), nullptr);
    TEST_ASSERT_EQ(packets[0].severity, static_cast<uint8_t>(HitSeverity::DirectHit));
    TEST_ASSERT((packets[0].hit_flags & HitFlags::DirectHit) != 0);

    entries[0].hit_severity = 0x02;
    packets = decoder::decode_action_effects(1001, header, entries.data(), nullptr);
    TEST_ASSERT_EQ(packets[0].severity, static_cast<uint8_t>(HitSeverity::Normal));

    // Crit Direct Hit (0x20 | 0x40 = 0x60)
    entries[0].hit_severity = 0x60; // Real FFXIV client CDH flag
    packets = decoder::decode_action_effects(1001, header, entries.data(), nullptr);
    TEST_ASSERT_EQ(packets[0].severity, static_cast<uint8_t>(HitSeverity::CritDirectHit));
    TEST_ASSERT((packets[0].hit_flags & HitFlags::Crit) != 0);
    TEST_ASSERT((packets[0].hit_flags & HitFlags::DirectHit) != 0);

    entries[0].hit_severity = 0x03;
    packets = decoder::decode_action_effects(1001, header, entries.data(), nullptr);
    TEST_ASSERT_EQ(packets[0].severity, static_cast<uint8_t>(HitSeverity::Normal));

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
    entries[0].param = 0x20; // A heal's crit is in byte 2

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

TEST_CASE(MeterDecoder, HealCritComesFromTheParamByte) {
    // The client prints "Critical!" for a heal on byte 2's 0x20 and reads byte 1 only
    // for damage, so the old byte-1 read never counted a heal crit.
    hub::game::ActionEffectHeader header{};
    header.animation_target_id = 0x1000;
    header.action_id = 120; // Cure
    header.num_targets = 1;

    std::array<hub::game::ActionEffectEntry, 8> entries{};
    entries[0].effect_type = 0x04;
    entries[0].value = 18000;
    entries[0].hit_severity = 0x60;
    auto packets = decoder::decode_action_effects(2001, header, entries.data(), nullptr);
    TEST_ASSERT_EQ(packets.size(), 1u);
    TEST_ASSERT_EQ(packets[0].severity, static_cast<uint8_t>(HitSeverity::Normal));
    TEST_ASSERT((packets[0].hit_flags & HitFlags::Crit) == 0);
    TEST_ASSERT((packets[0].hit_flags & HitFlags::DirectHit) == 0);

    // Damage keeps both in byte 1; its byte 2 is attack type and element.
    entries[0] = {};
    entries[0].effect_type = 0x03;
    entries[0].value = 9000;
    entries[0].param = 0x20;
    packets = decoder::decode_action_effects(2001, header, entries.data(), nullptr);
    TEST_ASSERT_EQ(packets[0].severity, static_cast<uint8_t>(HitSeverity::Normal));
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

TEST_CASE(MeterEngine, ZoneOnlyAnnouncementOfZeroClearsZone) {
    EncounterEngine engine;
    const auto t0 = std::chrono::steady_clock::now();

    hub::ipc::EncounterControlPacket zone{};
    zone.zone_id = 1000;
    engine.process_encounter_control(zone, t0);

    // Going solo after a duty: the zone becomes unknown instead of sticking.
    zone.zone_id = 0;
    engine.process_encounter_control(zone, t0 + std::chrono::seconds(1));
    TEST_ASSERT_EQ(engine.current_zone_id(), 0u);
    TEST_ASSERT_EQ(engine.state(), EncounterState::Idle);

    hub::ipc::CombatActionPacket act{};
    act.source_id = 1;
    act.damage = 4200;
    act.effect_type = static_cast<uint16_t>(EffectType::Damage);
    engine.process_action(act, t0 + std::chrono::seconds(2));
    engine.end_encounter(EncounterEndReason::Manual, t0 + std::chrono::seconds(3));

    const auto pull = engine.latest_pull();
    TEST_ASSERT(pull.has_value());
    TEST_ASSERT_EQ(pull->zone_id, 0u);
}

TEST_CASE(MeterEngine, ControlPacketWithCommandIgnoresZeroZone) {
    EncounterEngine engine;
    const auto t0 = std::chrono::steady_clock::now();

    hub::ipc::EncounterControlPacket zone{};
    zone.zone_id = 1000;
    engine.process_encounter_control(zone, t0);

    // A combat-start packet carries no zone; its 0 must not clear the known one.
    hub::ipc::EncounterControlPacket start{};
    start.in_combat_flag = 1;
    engine.process_encounter_control(start, t0 + std::chrono::seconds(1));
    TEST_ASSERT_EQ(engine.current_zone_id(), 1000u);
    TEST_ASSERT_EQ(engine.state(), EncounterState::InCombat);
}

TEST_CASE(MeterEngine, UnknownZoneMidPullDoesNotSplit) {
    EncounterEngine engine;
    const auto t0 = std::chrono::steady_clock::now();
    engine.set_zone(1000, "", t0);

    hub::ipc::CombatActionPacket act{};
    act.source_id = 1;
    act.damage = 4200;
    act.effect_type = static_cast<uint16_t>(EffectType::Damage);
    engine.process_action(act, t0);
    TEST_ASSERT_EQ(engine.state(), EncounterState::InCombat);

    // The party disbanding mid-pull hides the zone but the player has not moved.
    engine.set_zone(0, "", t0 + std::chrono::seconds(1));
    TEST_ASSERT_EQ(engine.state(), EncounterState::InCombat);
    TEST_ASSERT(!engine.latest_pull().has_value());

    engine.end_encounter(EncounterEndReason::Manual, t0 + std::chrono::seconds(2));
    const auto pull = engine.latest_pull();
    TEST_ASSERT(pull.has_value());
    TEST_ASSERT_EQ(pull->zone_id, 1000u);
    TEST_ASSERT_EQ(engine.current_zone_id(), 0u);
}

TEST_CASE(MeterEngine, UnknownZoneMidPullAppliesOnReset) {
    EncounterEngine engine;
    const auto t0 = std::chrono::steady_clock::now();
    engine.set_zone(1000, "", t0);

    hub::ipc::CombatActionPacket act{};
    act.source_id = 1;
    act.damage = 4200;
    act.effect_type = static_cast<uint16_t>(EffectType::Damage);
    engine.process_action(act, t0);

    engine.set_zone(0, "", t0 + std::chrono::seconds(1));
    engine.reset_current();
    TEST_ASSERT_EQ(engine.current_zone_id(), 0u);
}

namespace {

/// Opens a pull with one hit at `at` and ends it by hand two seconds later.
void archive_pull(EncounterEngine& engine, std::chrono::steady_clock::time_point at) {
    hub::ipc::CombatActionPacket act{};
    act.source_id = 1;
    act.damage = 4200;
    act.effect_type = static_cast<uint16_t>(EffectType::Damage);
    engine.process_action(act, at);
    engine.end_encounter(EncounterEndReason::Manual, at + std::chrono::seconds(2));
}

} // namespace

TEST_CASE(MeterEngine, PullNumbersStartOverInEachZoneVisit) {
    // The rail numbers pulls from #1 in each zone, and a zone entered again is a
    // new entry rather than more pulls in the old one.
    EncounterEngine engine;
    const auto t0 = std::chrono::steady_clock::now();
    const auto at = [t0](int s) { return t0 + std::chrono::seconds(s); };
    engine.set_zone(1000, "", at(0));
    archive_pull(engine, at(10));
    archive_pull(engine, at(20));
    engine.set_zone(1001, "", at(30));
    archive_pull(engine, at(40));
    engine.set_zone(1000, "", at(50));
    archive_pull(engine, at(60));

    const auto index = engine.pull_history_index();
    TEST_ASSERT_EQ(index.size(), 4u);
    TEST_ASSERT_EQ(index[0].pull_number, 1u);
    TEST_ASSERT_EQ(index[1].pull_number, 2u);
    TEST_ASSERT_EQ(index[2].pull_number, 1u);
    TEST_ASSERT_EQ(index[3].pull_number, 1u);
    TEST_ASSERT_EQ(index[0].zone_visit, index[1].zone_visit);
    TEST_ASSERT(index[2].zone_visit != index[1].zone_visit);
    TEST_ASSERT_EQ(index[3].zone_id, index[0].zone_id);
    TEST_ASSERT(index[3].zone_visit != index[0].zone_visit);
    // Selection keys on encounter_id, which still counts every pull.
    TEST_ASSERT_EQ(index[3].encounter_id, 4u);
}

TEST_CASE(MeterEngine, ZoneChangeFilesThePullUnderItsVisit) {
    EncounterEngine engine;
    const auto t0 = std::chrono::steady_clock::now();
    const auto at = [t0](int s) { return t0 + std::chrono::seconds(s); };
    engine.set_zone(1000, "", at(0));

    hub::ipc::CombatActionPacket act{};
    act.source_id = 1;
    act.damage = 4200;
    act.effect_type = static_cast<uint16_t>(EffectType::Damage);
    engine.process_action(act, at(1));
    engine.set_zone(1001, "", at(3));
    archive_pull(engine, at(10));

    const auto index = engine.pull_history_index();
    TEST_ASSERT_EQ(index.size(), 2u);
    TEST_ASSERT_EQ(index[0].end_reason, EncounterEndReason::ZoneChange);
    TEST_ASSERT_EQ(index[0].zone_id, 1000u);
    TEST_ASSERT_EQ(index[0].pull_number, 1u);
    TEST_ASSERT_EQ(index[1].zone_id, 1001u);
    TEST_ASSERT_EQ(index[1].pull_number, 1u);
    TEST_ASSERT(index[1].zone_visit != index[0].zone_visit);
}

TEST_CASE(MeterEngine, GoingUnknownMidPullNumbersTheNextPullFromOne) {
    // The pull the zone went unknown in stays in its visit; the unknown zone's own
    // visit only begins once that pull is archived.
    EncounterEngine engine;
    const auto t0 = std::chrono::steady_clock::now();
    const auto at = [t0](int s) { return t0 + std::chrono::seconds(s); };
    engine.set_zone(1000, "", at(0));
    archive_pull(engine, at(1));

    hub::ipc::CombatActionPacket act{};
    act.source_id = 1;
    act.damage = 4200;
    act.effect_type = static_cast<uint16_t>(EffectType::Damage);
    engine.process_action(act, at(10));
    engine.set_zone(0, "", at(11));
    engine.end_encounter(EncounterEndReason::Manual, at(12));
    archive_pull(engine, at(20));

    const auto index = engine.pull_history_index();
    TEST_ASSERT_EQ(index.size(), 3u);
    TEST_ASSERT_EQ(index[1].zone_id, 1000u);
    TEST_ASSERT_EQ(index[1].pull_number, 2u);
    TEST_ASSERT_EQ(index[1].zone_visit, index[0].zone_visit);
    TEST_ASSERT_EQ(index[2].zone_id, 0u);
    TEST_ASSERT_EQ(index[2].pull_number, 1u);
    TEST_ASSERT(index[2].zone_visit != index[1].zone_visit);
}

TEST_CASE(MeterEngine, UnknownZoneKeepsOnlyItsNewestPull) {
    // Solo there is no zone to tell pulls apart, so only the newest is kept, and a
    // run of them never costs a duty's pull its place in the history.
    EncounterEngine engine;
    engine.set_history_capacity(3);
    const auto t0 = std::chrono::steady_clock::now();
    const auto at = [t0](int s) { return t0 + std::chrono::seconds(s); };
    engine.set_zone(1000, "", at(0));
    archive_pull(engine, at(10));
    archive_pull(engine, at(20));
    engine.set_zone(0, "", at(30));
    for (int i = 0; i < 4; ++i) {
        archive_pull(engine, at(40 + 10 * i));
    }

    auto index = engine.pull_history_index();
    TEST_ASSERT_EQ(index.size(), 3u);
    TEST_ASSERT_EQ(index[0].zone_id, 1000u);
    TEST_ASSERT_EQ(index[1].zone_id, 1000u);
    TEST_ASSERT_EQ(index[2].zone_id, 0u);
    // The newest, still numbered in its visit.
    TEST_ASSERT_EQ(index[2].pull_number, 4u);
    TEST_ASSERT_EQ(index[2].encounter_id, 6u);
    TEST_ASSERT_EQ(engine.latest_pull()->encounter_id, 6u);

    // An unknown pull from an earlier visit goes too.
    engine.set_zone(1001, "", at(100));
    engine.set_zone(0, "", at(110));
    archive_pull(engine, at(120));
    index = engine.pull_history_index();
    TEST_ASSERT_EQ(index.size(), 3u);
    TEST_ASSERT_EQ(index[0].zone_id, 1000u);
    TEST_ASSERT_EQ(index[2].encounter_id, 7u);
    TEST_ASSERT_EQ(index[2].pull_number, 1u);
}

TEST_CASE(MeterEngine, ClearingHistoryRestartsNumbering) {
    EncounterEngine engine;
    const auto t0 = std::chrono::steady_clock::now();
    const auto at = [t0](int s) { return t0 + std::chrono::seconds(s); };
    engine.set_zone(1000, "", at(0));
    archive_pull(engine, at(10));
    archive_pull(engine, at(20));
    engine.clear_history();
    archive_pull(engine, at(30));

    const auto index = engine.pull_history_index();
    TEST_ASSERT_EQ(index.size(), 1u);
    TEST_ASSERT_EQ(index[0].pull_number, 1u);
    // A stale selection must never land on it.
    TEST_ASSERT_EQ(index[0].encounter_id, 3u);
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
    // Duties are written mid-sentence in the sheet; a label is a title.
    TEST_ASSERT(zone_label(0, "the Aurum Vale") == "The Aurum Vale");
    // The hyphen is a macro in the sheet, not a literal character.
    TEST_ASSERT(zone_label(1037, "") == "The Tam-Tara Deepcroft");
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
    // In a zone: an unknown zone keeps only its newest pull.
    engine.set_zone(1000);

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

TEST_CASE(MeterEngine, LoweringTheHistoryCapacityDropsTheOldest) {
    // Lowering it used to wait for the next pull's end, then drop all the excess at once.
    EncounterEngine engine;
    TEST_ASSERT_EQ(engine.history_capacity(), 100u);
    const auto t0 = std::chrono::steady_clock::now();
    const auto at = [t0](int s) { return t0 + std::chrono::seconds(s); };
    engine.set_zone(1000, "", at(0));
    for (int i = 0; i < 5; ++i) {
        archive_pull(engine, at(10 + 10 * i));
    }

    engine.set_history_capacity(2);
    auto index = engine.pull_history_index();
    TEST_ASSERT_EQ(index.size(), 2u);
    TEST_ASSERT_EQ(index[0].encounter_id, 4u);
    TEST_ASSERT_EQ(index[1].encounter_id, 5u);

    // Never below one: the live view and late deaths use the newest.
    engine.set_history_capacity(0);
    TEST_ASSERT_EQ(engine.history_capacity(), 1u);
    index = engine.pull_history_index();
    TEST_ASSERT_EQ(index.size(), 1u);
    TEST_ASSERT_EQ(index[0].encounter_id, 5u);
    TEST_ASSERT_EQ(engine.latest_pull()->encounter_id, 5u);
    // Out of combat the live view takes its duration from the newest archived pull.
    TEST_ASSERT_NEAR(engine.current_summary(at(100)).duration_seconds, 2.0, 0.01);
}

TEST_CASE(MeterPlugin, InGameEngineKeepsOnlyTheLatestPull) {
    // Nothing in-game reads past the latest pull, so the game holds no archive.
    CombatPlugin plugin;
    plugin.initialize();
    EncounterEngine& engine = plugin.engine();
    const auto t0 = std::chrono::steady_clock::now();
    const auto at = [t0](int s) { return t0 + std::chrono::seconds(s); };
    engine.set_zone(1000, "", at(0));
    for (int i = 0; i < 3; ++i) {
        archive_pull(engine, at(10 + 10 * i));
    }

    const auto index = engine.pull_history_index();
    TEST_ASSERT_EQ(index.size(), 1u);
    TEST_ASSERT_EQ(index[0].encounter_id, 3u);
    TEST_ASSERT_EQ(index[0].pull_number, 3u);
    plugin.shutdown();
}

TEST_CASE(MeterPlugin, PluginLifecycleAndConfig) {
    CombatPlugin plugin;
    TEST_ASSERT_EQ(plugin.id(), hub::PluginId::CombatMeter);
    TEST_ASSERT_EQ(std::string(plugin.name()), "Combat Meter");

    TEST_ASSERT_TRUE(plugin.initialize());

    // Serialize default config to JSON
    hub::config::JsonValue json{hub::config::JsonValue::ObjectType{}};
    plugin.serialize_config(json);
    TEST_ASSERT_TRUE(json["plugin_enabled"].as_bool(false));
    TEST_ASSERT_NEAR(json["inactivity_timeout_seconds"].as_double(0.0), 7.0, 0.01);
    TEST_ASSERT_FALSE(json["party_only"].as_bool(true));

    // Modify and deserialize back
    json["inactivity_timeout_seconds"] = hub::config::JsonValue(10.0);
    json["party_only"] = hub::config::JsonValue(true);
    json["overlay_width"] = hub::config::JsonValue(950);
    plugin.deserialize_config(json);

    TEST_ASSERT_NEAR(plugin.config().inactivity_timeout_seconds, 10.0, 0.01);
    TEST_ASSERT_NEAR(plugin.engine().inactivity_timeout(), 10.0, 0.01);
    TEST_ASSERT_TRUE(plugin.config().party_only);
    TEST_ASSERT_NEAR(plugin.config().overlay.width, 950.0f, 0.01f);

    // The master switch round-trips under the shared key, and the legacy
    // "enabled" key an older config.json carries is still honoured.
    plugin.set_enabled(false);
    hub::config::JsonValue off{hub::config::JsonValue::ObjectType{}};
    plugin.serialize_config(off);
    TEST_ASSERT_FALSE(off["plugin_enabled"].as_bool(true));
    plugin.set_enabled(true);
    plugin.deserialize_config(off);
    TEST_ASSERT_FALSE(plugin.is_enabled());

    hub::config::JsonValue legacy{hub::config::JsonValue::ObjectType{}};
    legacy["enabled"] = hub::config::JsonValue(false);
    plugin.set_enabled(true);
    plugin.deserialize_config(legacy);
    TEST_ASSERT_FALSE(plugin.is_enabled());
    plugin.set_enabled(true);

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

    // What the hook sends for effect kind 3, a damage-over-time tick.
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
    plugin.set_enabled(false);

    plugin.on_status_tick(0x40000123, 777, 1871, 4500, false);
    TEST_ASSERT_FALSE(plugin.engine().in_combat());

    plugin.set_enabled(true);
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

TEST_CASE(MeterPlugin, StreamsNothingWhileDisconnected) {
    // Packets queued with no app listening reached the next app all at once, and
    // its engine booked the old fight as a pull a few milliseconds long.
    CombatPlugin plugin;
    plugin.initialize();
    hub::ipc::PacketRingBuffer ring;
    plugin.set_ring_buffer(&ring);
    plugin.set_connected(false);

    hub::game::ActionEffectHeader header{};
    header.animation_target_id = 0x40001;
    header.action_id = 31;
    header.num_targets = 1;
    std::array<hub::game::ActionEffectEntry, 8> entries{};
    entries[0].effect_type = 0x03; // Damage
    entries[0].value = 25000;

    plugin.on_receive_action_effect(777, nullptr, &header, entries.data(), nullptr);
    plugin.on_status_tick(0x40001, 777, 1871, 4500, /*is_heal=*/false);
    ActorVitals vitals{};
    vitals.entity = 777;
    vitals.hp = 100;
    vitals.max_hp = 100;
    vitals.track_life = true;
    vitals.statuses_read = true;
    plugin.on_vitals(std::span<const ActorVitals>(&vitals, 1), 1'000'000);

    std::vector<uint8_t> item;
    TEST_ASSERT_FALSE(ring.pop(item));
    // The in-game meter keeps counting.
    TEST_ASSERT_EQ(plugin.engine().accumulator().total_damage(), 29500u);

    plugin.set_connected(true);
    plugin.on_receive_action_effect(777, nullptr, &header, entries.data(), nullptr);
    TEST_ASSERT_TRUE(ring.pop(item));
}

TEST_CASE(MeterPlugin, DisablingMidPullClearsPacketCombat) {
    // update() returned before publishing the combat bit once switched off, so a
    // pull in progress left "in combat" set for every overlay.
    CombatPlugin plugin;
    plugin.initialize();
    hub::GameStateProvider game_state;
    plugin.set_game_state(&game_state);

    hub::game::ActionEffectHeader header{};
    header.animation_target_id = 0x40001;
    header.action_id = 31;
    header.num_targets = 1;
    std::array<hub::game::ActionEffectEntry, 8> entries{};
    entries[0].effect_type = 0x03; // Damage
    entries[0].value = 25000;

    plugin.on_receive_action_effect(777, nullptr, &header, entries.data(), nullptr);
    plugin.update(0.05);
    TEST_ASSERT_TRUE(game_state.has(hub::GameStateFlag::InCombat));

    plugin.set_enabled(false);
    plugin.update(0.05);
    TEST_ASSERT_FALSE(game_state.has(hub::GameStateFlag::InCombat));
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
        while (!stop.load()) {
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

TEST_CASE(MeterAccumulator, HealTickOverhealAccounting) {
    MetricsAccumulator acc;
    CombatantRegistry reg;
    reg.register_actor(10, "White Mage", Job::WHM, 0, ActorType::Player);
    reg.register_actor(20, "Warrior", Job::WAR, 0, ActorType::Player);

    hub::ipc::StatusTickPacket tick{};
    tick.source_id = 10;
    tick.target_id = 20;
    tick.status_id = 158; // Regen
    tick.effect_type = static_cast<uint8_t>(EffectType::Heal);
    tick.damage_or_heal = 2000;
    tick.overheal = 1500;
    acc.record_status_tick(tick, reg);

    tick.overheal = 2000; // Landed on a target already at full
    acc.record_status_tick(tick, reg);

    const auto* whm = acc.find_stats(10);
    TEST_ASSERT(whm != nullptr);
    TEST_ASSERT_EQ(whm->total_healing, 4000u);
    TEST_ASSERT_EQ(whm->effective_healing, 500u);
    TEST_ASSERT_EQ(whm->overhealing, 3500u);
    TEST_ASSERT_EQ(acc.total_effective_healing(), 500u);
    TEST_ASSERT_EQ(acc.total_overhealing(), 3500u);

    const auto& regen = whm->actions.at(158 | STATUS_ACTION_KEY_OFFSET);
    TEST_ASSERT_EQ(regen.effective_healing, 500u);
    TEST_ASSERT_EQ(regen.overhealing, 3500u);
    TEST_ASSERT_EQ(regen.min_heal, 0u);
    TEST_ASSERT_EQ(regen.max_heal, 500u);
    TEST_ASSERT_EQ(regen.heal_hit_counts.tick_hits, 2u);

    // Only the tick that landed reaches the recap, as with direct heals.
    const auto* recap = acc.recap_for(20);
    TEST_ASSERT(recap != nullptr);
    TEST_ASSERT_EQ(recap->size(), 1u);
    TEST_ASSERT_EQ(recap->at(0).amount, 500u);
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

    // The mitigator's action feed branches on the raw accessor being empty rather
    // than on that fallback text, so pin the empty-view contract directly.
    TEST_ASSERT_TRUE(hub::game::action_sheet_name(999999).empty());
    TEST_ASSERT(hub::game::action_sheet_name(36954) == "Lance Barrage");
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

namespace {

PullHistoryEntry archived_entry(uint32_t zone, uint32_t visit) {
    PullHistoryEntry entry{};
    entry.zone_id = zone;
    entry.zone_visit = visit;
    return entry;
}

} // namespace

TEST_CASE(MeterPullGrouping, GroupsVisitsNewestFirst) {
    // Archive order is chronological, so the last entry is the newest pull.
    const std::vector<PullHistoryEntry> history = {
        archived_entry(1238, 1), archived_entry(1238, 1), archived_entry(1, 2),
        archived_entry(1238, 3), archived_entry(1238, 3),
    };

    const auto groups = group_pulls_by_visit(history);
    TEST_ASSERT_EQ(groups.size(), 3u);

    // The newest visit (index 4) leads.
    TEST_ASSERT_EQ(groups[0].zone_visit, 3u);
    TEST_ASSERT_EQ(groups[0].pulls, (std::vector<size_t>{4, 3}));
    TEST_ASSERT_EQ(groups[1].zone_id, 1u);
    TEST_ASSERT_EQ(groups[1].pulls, (std::vector<size_t>{2}));
    TEST_ASSERT_EQ(groups[2].zone_visit, 1u);
    TEST_ASSERT_EQ(groups[2].pulls, (std::vector<size_t>{1, 0}));
}

TEST_CASE(MeterPullGrouping, RevisitedZoneIsItsOwnGroup) {
    // Leaving a duty and coming back is a new entry, whose pulls number from #1.
    const std::vector<PullHistoryEntry> history = {
        archived_entry(1238, 1), archived_entry(1, 2), archived_entry(1, 2), archived_entry(1238, 3),
    };

    const auto groups = group_pulls_by_visit(history);
    TEST_ASSERT_EQ(groups.size(), 3u);
    TEST_ASSERT_EQ(groups[0].zone_id, 1238u);
    TEST_ASSERT_EQ(groups[0].pulls, (std::vector<size_t>{3}));
    TEST_ASSERT_EQ(groups[1].pulls, (std::vector<size_t>{2, 1}));
    TEST_ASSERT_EQ(groups[2].zone_id, 1238u);
    TEST_ASSERT_EQ(groups[2].pulls, (std::vector<size_t>{0}));
    TEST_ASSERT_EQ(groups[0].label, groups[2].label);
}

TEST_CASE(MeterPullGrouping, EveryGroupCarriesALabel) {
    std::vector<PullHistoryEntry> history = {
        archived_entry(0, 0),             // never entered a zone
        archived_entry(1238, 1),          // resolved from the territory table
        archived_entry(999999, 2),        // not in the sheet
    };
    history[2].zone_name = "Somewhere";   // explicit name wins

    const auto groups = group_pulls_by_visit(history);
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
    engine.set_zone(1000, "", t);
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

// ---------------------------------------------------------------------------
// Actor info reaching the desktop app. The app runs its own engine and learns a
// name only from an ActorInfo packet, so anything the payload resolves locally
// but never publishes shows up there as Entity_<id> - and, once a pull closes,
// stays that way in the archive.
// ---------------------------------------------------------------------------

namespace {

/// Every ActorInfo packet sitting in a ring buffer, decoded.
std::vector<hub::ipc::ActorInfoPacket> drain_actor_info(hub::ipc::PacketRingBuffer& ring) {
    std::vector<hub::ipc::ActorInfoPacket> out;
    std::vector<uint8_t> frame;
    while (ring.pop(frame)) {
        const auto header = hub::ipc::deserialize_header(frame);
        if (!header) continue;
        if (header->message_type != static_cast<uint16_t>(hub::MessageType::CombatActorInfo)) continue;
        if (frame.size() < sizeof(hub::ipc::PacketHeader) + sizeof(hub::ipc::ActorInfoPacket)) continue;
        hub::ipc::ActorInfoPacket actor{};
        std::memcpy(&actor, frame.data() + sizeof(hub::ipc::PacketHeader), sizeof(actor));
        out.push_back(actor);
    }
    return out;
}

/// Wires a plugin the way dllmain does, so the source-character path publishes
/// instead of only registering locally.
void attach_object_resolver(CombatPlugin& plugin, hub::payload::ObjectReader& reader) {
    plugin.set_actor_object_resolver([&](const void* character) {
        plugin.engine().with_registry([&](CombatantRegistry& registry) {
            reader.inspect_and_sync_actor_direct(character, &registry);
        });
    });
}

} // namespace

TEST_CASE(MeterPlugin, PublishesActorInfoForSourceWithCharacterPointer) {
    // The hook usually does hand over a source character, and that branch used
    // to register the name in-process only: the in-game overlay showed it while
    // the desktop app showed Entity_<id> for the very same pull.
    hub::ipc::PacketRingBuffer ring;
    hub::payload::ObjectReader reader(&ring);

    CombatPlugin plugin;
    plugin.initialize();
    plugin.set_ring_buffer(&ring);
    attach_object_resolver(plugin, reader);

    hub::game::ActionEffectHeader header{};
    header.animation_target_id = 0x40001;
    header.action_id = 31;
    header.num_targets = 1;

    std::array<hub::game::ActionEffectEntry, 8> entries{};
    entries[0].effect_type = 0x03; // Damage
    entries[0].value = 25000;

    hub::game::CharacterObject chr{};
    chr.entity_id = 777;
    const std::string test_name = "Krile Baldesion";
    std::copy(test_name.begin(), test_name.end(), chr.name);
    chr.class_job = static_cast<uint8_t>(Job::PCT);
    chr.object_kind = 1;       // Player
    chr.owner_id = 0xE0000000; // Game's "no owner" sentinel
    chr.current_hp = 50000;
    chr.max_hp = 50000;

    plugin.on_receive_action_effect(777, &chr, &header, entries.data(), nullptr);

    // Still registered locally for the in-game overlay.
    const auto* actor = plugin.engine().registry().find_actor(777);
    TEST_ASSERT(actor != nullptr);
    TEST_ASSERT_EQ(actor->name, test_name);

    const auto published = drain_actor_info(ring);
    const hub::ipc::ActorInfoPacket* source = nullptr;
    for (const auto& p : published) {
        if (p.entity_id == 777) source = &p;
    }
    TEST_ASSERT(source != nullptr);
    TEST_ASSERT_EQ(std::string(source->name), test_name);
    TEST_ASSERT_EQ(source->job_id, static_cast<uint32_t>(Job::PCT));
    TEST_ASSERT_EQ(source->actor_type, static_cast<uint8_t>(ActorType::Player));
    TEST_ASSERT_EQ(source->owner_id, 0u);

    plugin.shutdown();
}

TEST_CASE(MeterPlugin, RepublishesActorInfoOnlyWhenItChanges) {
    // ReceiveActionEffect fires several times a second per actor, so the
    // publishing path has to dedupe or it floods the pipe.
    hub::ipc::PacketRingBuffer ring;
    hub::payload::ObjectReader reader(&ring);

    CombatPlugin plugin;
    plugin.initialize();
    plugin.set_ring_buffer(&ring);
    attach_object_resolver(plugin, reader);

    hub::game::ActionEffectHeader header{};
    header.animation_target_id = 0x40001;
    header.action_id = 31;
    header.num_targets = 1;

    std::array<hub::game::ActionEffectEntry, 8> entries{};
    entries[0].effect_type = 0x03;
    entries[0].value = 1000;

    hub::game::CharacterObject chr{};
    chr.entity_id = 777;
    const std::string test_name = "Krile";
    std::copy(test_name.begin(), test_name.end(), chr.name);
    chr.class_job = static_cast<uint8_t>(Job::PCT);
    chr.object_kind = 1;
    chr.owner_id = 0xE0000000;
    chr.current_hp = 50000;
    chr.max_hp = 50000;

    for (int i = 0; i < 5; ++i) {
        plugin.on_receive_action_effect(777, &chr, &header, entries.data(), nullptr);
    }

    size_t for_source = 0;
    for (const auto& p : drain_actor_info(ring)) {
        if (p.entity_id == 777) ++for_source;
    }
    TEST_ASSERT_EQ(for_source, 1u);

    // A job change is new information and has to get through.
    chr.class_job = static_cast<uint8_t>(Job::WAR);
    plugin.on_receive_action_effect(777, &chr, &header, entries.data(), nullptr);

    for_source = 0;
    for (const auto& p : drain_actor_info(ring)) {
        if (p.entity_id == 777) ++for_source;
    }
    TEST_ASSERT_EQ(for_source, 1u);

    plugin.shutdown();
}

TEST_CASE(MeterPlugin, ArchivedPullCarriesNameIntoMirrorEngine) {
    // End to end over the wire, without a game: what the payload publishes has
    // to be enough for the app's engine to archive a real name. The archive is a
    // value snapshot, so a name missing when the pull closes is missing forever.
    hub::ipc::PacketRingBuffer ring;
    hub::payload::ObjectReader reader(&ring);

    CombatPlugin plugin;
    plugin.initialize();
    plugin.set_ring_buffer(&ring);
    attach_object_resolver(plugin, reader);

    hub::game::ActionEffectHeader header{};
    header.animation_target_id = 0x40001;
    header.action_id = 31;
    header.num_targets = 1;

    std::array<hub::game::ActionEffectEntry, 8> entries{};
    entries[0].effect_type = 0x03;
    entries[0].value = 25000;

    hub::game::CharacterObject chr{};
    chr.entity_id = 777;
    const std::string test_name = "Krile";
    std::copy(test_name.begin(), test_name.end(), chr.name);
    chr.class_job = static_cast<uint8_t>(Job::PCT);
    chr.object_kind = 1;
    chr.owner_id = 0xE0000000;
    chr.current_hp = 50000;
    chr.max_hp = 50000;

    // Solo: no party sync, so the party list cannot supply the name either.
    plugin.on_receive_action_effect(777, &chr, &header, entries.data(), nullptr);

    // Replay everything the payload put on the wire into the app's own engine.
    EncounterEngine mirror;
    std::vector<uint8_t> frame;
    while (ring.pop(frame)) {
        const auto hdr = hub::ipc::deserialize_header(frame);
        if (!hdr) continue;
        const auto* body = frame.data() + sizeof(hub::ipc::PacketHeader);
        if (hdr->message_type == static_cast<uint16_t>(hub::MessageType::CombatActorInfo)) {
            hub::ipc::ActorInfoPacket actor{};
            std::memcpy(&actor, body, sizeof(actor));
            mirror.process_actor_info(actor);
        } else if (hdr->message_type == static_cast<uint16_t>(hub::MessageType::CombatAction)) {
            hub::ipc::CombatActionPacket action{};
            std::memcpy(&action, body, sizeof(action));
            mirror.process_action(action);
        }
    }

    mirror.end_encounter(EncounterEndReason::Manual);

    const auto history = mirror.pull_history();
    TEST_ASSERT_EQ(history.size(), 1u);

    const CombatantStats* row = nullptr;
    for (const auto& c : history[0].combatants) {
        if (c.entity_id == 777) row = &c;
    }
    TEST_ASSERT(row != nullptr);
    TEST_ASSERT_EQ(row->name, test_name);
    TEST_ASSERT_EQ(row->job, Job::PCT);

    plugin.shutdown();
}

TEST_CASE(MeterRegistry, LocalPlayerFollowsTheCurrentParty) {
    // The id used to be latched on the first sync and never revisited, so it
    // outlived the party it came from and kept flagging a stranger.
    CombatantRegistry reg;

    hub::ipc::PartySyncPacket first{};
    first.party_count = 2;
    first.local_player_id = 1001;
    first.entity_ids[0] = 1001; first.job_ids[0] = static_cast<uint32_t>(Job::WAR);
    first.entity_ids[1] = 1002; first.job_ids[1] = static_cast<uint32_t>(Job::WHM);
    reg.sync_party(first);
    TEST_ASSERT_EQ(reg.local_player_id(), 1001u);

    hub::ipc::PartySyncPacket second{};
    second.party_count = 2;
    second.local_player_id = 2001;
    second.entity_ids[0] = 2001; second.job_ids[0] = static_cast<uint32_t>(Job::PCT);
    second.entity_ids[1] = 2002; second.job_ids[1] = static_cast<uint32_t>(Job::SGE);
    reg.sync_party(second);
    TEST_ASSERT_EQ(reg.local_player_id(), 2001u);

    const auto* stale = reg.find_actor(1001);
    TEST_ASSERT(stale != nullptr);
    TEST_ASSERT_FALSE(stale->is_local_player);

    // An unknown id (signature missed, or the game's placeholder) leaves nobody flagged.
    hub::ipc::PartySyncPacket unknown{};
    unknown.local_player_id = hub::game::NO_ENTITY_ID;
    reg.sync_party(unknown);
    TEST_ASSERT_EQ(reg.local_player_id(), 0u);
}

TEST_CASE(MeterRegistry, LocalPlayerIsNotSlotZero) {
    // The client fills the party list in server order; the local player can be any slot.
    CombatantRegistry reg;

    hub::ipc::PartySyncPacket party{};
    party.party_count = 3;
    party.local_player_id = 3003;
    party.entity_ids[0] = 3001; party.job_ids[0] = static_cast<uint32_t>(Job::PLD);
    party.entity_ids[1] = 3002; party.job_ids[1] = static_cast<uint32_t>(Job::AST);
    party.entity_ids[2] = 3003; party.job_ids[2] = static_cast<uint32_t>(Job::SMN);
    reg.sync_party(party);
    TEST_ASSERT_EQ(reg.local_player_id(), 3003u);
    TEST_ASSERT_FALSE(reg.find_actor(3001)->is_local_player);
    TEST_ASSERT(reg.find_actor(3003)->is_local_player);

    // Solo play has no party list but still knows who we are.
    hub::ipc::PartySyncPacket solo{};
    solo.local_player_id = 3003;
    reg.sync_party(solo);
    TEST_ASSERT_EQ(reg.local_player_id(), 3003u);
    TEST_ASSERT(reg.find_actor(3003)->is_local_player);
}

TEST_CASE(MeterRegistry, PartySyncClearsFormerMembers) {
    // The flag used to be set on current members only, so anyone who left kept it.
    CombatantRegistry reg;

    hub::ipc::PartySyncPacket both{};
    both.party_count = 2;
    both.entity_ids[0] = 1001; both.job_ids[0] = static_cast<uint32_t>(Job::WAR);
    both.entity_ids[1] = 1002; both.job_ids[1] = static_cast<uint32_t>(Job::WHM);
    reg.sync_party(both);
    TEST_ASSERT(reg.find_actor(1002)->is_party_member);

    hub::ipc::PartySyncPacket one{};
    one.party_count = 1;
    one.entity_ids[0] = 1001; one.job_ids[0] = static_cast<uint32_t>(Job::WAR);
    reg.sync_party(one);
    TEST_ASSERT(reg.find_actor(1001)->is_party_member);
    TEST_ASSERT_FALSE(reg.find_actor(1002)->is_party_member);

    hub::ipc::PartySyncPacket empty{};
    reg.sync_party(empty);
    TEST_ASSERT_FALSE(reg.find_actor(1001)->is_party_member);
}

TEST_CASE(MeterRegistry, RecycledPetIdIsNotAPet) {
    CombatantRegistry reg;
    reg.register_actor(10, "Summoner Player", Job::SMN);
    reg.register_actor(0x40000020, "Demi-Bahamut", Job::SMN, 10);
    TEST_ASSERT(reg.is_pet(0x40000020));

    // The game hands the id to a monster once the pet is gone.
    reg.register_actor(0x40000020, "Striking Dummy", Job::None, 0, ActorType::Monster);
    TEST_ASSERT_FALSE(reg.is_pet(0x40000020));
    TEST_ASSERT_EQ(reg.resolve_owner(0x40000020), 0x40000020u);
    TEST_ASSERT_FALSE(reg.is_friendly(0x40000020));

    // A pet re-read without owner info keeps the link it already had.
    reg.register_actor(0x40000030, "Demi-Bahamut", Job::SMN, 10);
    reg.register_actor(0x40000030, "Demi-Bahamut", Job::SMN, 0);
    TEST_ASSERT_EQ(reg.resolve_owner(0x40000030), 10u);
}

TEST_CASE(PayloadObjectReader, InvalidateCacheRepublishesEverything) {
    // A desktop app that reconnects mid-session has none of the names already
    // sent, and the dedupe cache would otherwise never offer them again.
    hub::ipc::PacketRingBuffer ring;
    hub::payload::ObjectReader reader(&ring);

    hub::game::CharacterObject chr{};
    chr.entity_id = 777;
    const std::string test_name = "Krile";
    std::copy(test_name.begin(), test_name.end(), chr.name);
    chr.class_job = static_cast<uint8_t>(Job::PCT);
    chr.object_kind = 1;
    chr.owner_id = 0xE0000000;
    chr.current_hp = 50000;
    chr.max_hp = 50000;

    reader.inspect_and_sync_actor_direct(&chr, nullptr);
    TEST_ASSERT_EQ(drain_actor_info(ring).size(), 1u);

    reader.inspect_and_sync_actor_direct(&chr, nullptr);
    TEST_ASSERT_EQ(drain_actor_info(ring).size(), 0u);

    reader.invalidate_cache();
    reader.inspect_and_sync_actor_direct(&chr, nullptr);

    const auto republished = drain_actor_info(ring);
    TEST_ASSERT_EQ(republished.size(), 1u);
    TEST_ASSERT_EQ(std::string(republished[0].name), test_name);
}

// ---------------------------------------------------------------------------
// What opens a pull. Prepull healing, shielding and buffing is preparation, and
// starting the clock on it meant the fight was already seconds old - with a
// healer's name at the top of a table nobody had hit anything in yet.
// ---------------------------------------------------------------------------

namespace {

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

TEST_CASE(MeterEngine, PrepullHealDoesNotStartEncounter) {
    EncounterEngine engine;

    engine.process_action(effect_packet(EffectType::Heal, 0, 8000));
    TEST_ASSERT_FALSE(engine.in_combat());

    // Nothing was banked either: the heal landed before the pull.
    TEST_ASSERT_EQ(engine.accumulator().total_healing(), 0u);

    // The first real hit is what opens it.
    engine.process_action(effect_packet(EffectType::Damage, 25000, 0));
    TEST_ASSERT_TRUE(engine.in_combat());
    TEST_ASSERT_EQ(engine.accumulator().total_damage(), 25000u);
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
    TEST_ASSERT_EQ(engine.accumulator().total_healing(), 8000u);
    TEST_ASSERT_TRUE(engine.in_combat());
}

// ---------------------------------------------------------------------------
// Review regressions: overheal, wipe detection, target slots, pet merges.
// ---------------------------------------------------------------------------

TEST_CASE(MeterDecoder, FirstBlockUsesTargetListNotAnimationTarget) {
    // A self-centred AoE animates on the caster; its first block hit an enemy.
    hub::game::ActionEffectHeader header{};
    header.animation_target_id = 1001;
    header.action_id = 139; // Holy
    header.num_targets = 2;

    uint64_t targets[2] = { 0x40000001, 0x40000002 };
    std::array<hub::game::ActionEffectEntry, 16> entries{};
    entries[0].effect_type = 0x03;
    entries[0].value = 9000;
    entries[8].effect_type = 0x03;
    entries[8].value = 8500;

    const auto packets = decoder::decode_action_effects(1001, header, entries.data(), targets);
    TEST_ASSERT_EQ(packets.size(), 2u);
    TEST_ASSERT_EQ(packets[0].target_id, 0x40000001u);
    TEST_ASSERT_EQ(packets[1].target_id, 0x40000002u);
}

TEST_CASE(MeterDecoder, HealOnSourceTargetsTheCaster) {
    // A drain's self-heal rides in the enemy's block with the on-source flag.
    hub::game::ActionEffectHeader header{};
    header.animation_target_id = 0x40000001;
    header.action_id = 7;
    header.num_targets = 1;

    std::array<hub::game::ActionEffectEntry, 8> entries{};
    entries[0].effect_type = 0x03;
    entries[0].value = 12000;
    entries[1].effect_type = 0x04;
    entries[1].value = 3000;
    entries[1].flags = 0x80;

    const auto packets = decoder::decode_action_effects(1001, header, entries.data(), nullptr);
    TEST_ASSERT_EQ(packets.size(), 2u);
    TEST_ASSERT_EQ(packets[0].target_id, 0x40000001u);
    TEST_ASSERT_EQ(packets[1].target_id, 1001u);
}

TEST_CASE(MeterDecoder, MpEffectsAreNotDecoded) {
    hub::game::ActionEffectHeader header{};
    header.animation_target_id = 1001;
    header.num_targets = 1;

    std::array<hub::game::ActionEffectEntry, 8> entries{};
    entries[0].effect_type = 0x0A; // MP loss
    entries[1].effect_type = 0x0B; // MP gain
    TEST_ASSERT_EQ(decoder::decode_action_effects(1001, header, entries.data(), nullptr).size(), 0u);
}

TEST_CASE(MeterDecoder, OverhealSplitsAgainstMissingHp) {
    hub::ipc::CombatActionPacket heal{};
    heal.effect_type = static_cast<uint16_t>(EffectType::Heal);
    heal.effective_heal = 20000;

    decoder::apply_overheal(heal, 95000, 100000);
    TEST_ASSERT_EQ(heal.effective_heal, 5000u);
    TEST_ASSERT_EQ(heal.overheal, 15000u);

    // A heal that fits entirely is all effective.
    hub::ipc::CombatActionPacket fits{};
    fits.effect_type = static_cast<uint16_t>(EffectType::Heal);
    fits.effective_heal = 20000;
    decoder::apply_overheal(fits, 50000, 100000);
    TEST_ASSERT_EQ(fits.effective_heal, 20000u);
    TEST_ASSERT_EQ(fits.overheal, 0u);

    // Unknown HP or a non-heal is left alone.
    hub::ipc::CombatActionPacket unknown = heal;
    unknown.effective_heal = 20000;
    unknown.overheal = 0;
    decoder::apply_overheal(unknown, 0, 0);
    TEST_ASSERT_EQ(unknown.effective_heal, 20000u);

    hub::ipc::CombatActionPacket hit{};
    hit.effect_type = static_cast<uint16_t>(EffectType::Damage);
    hit.damage = 20000;
    decoder::apply_overheal(hit, 95000, 100000);
    TEST_ASSERT_EQ(hit.overheal, 0u);
}

TEST_CASE(MeterDecoder, TickOverhealSplitsAgainstMissingHp) {
    hub::ipc::StatusTickPacket tick{};
    tick.effect_type = static_cast<uint8_t>(EffectType::Heal);
    tick.damage_or_heal = 20000;

    decoder::apply_overheal(tick, 95000, 100000);
    TEST_ASSERT_EQ(tick.damage_or_heal, 20000u);
    TEST_ASSERT_EQ(tick.overheal, 15000u);

    // A target already at full takes none of it.
    hub::ipc::StatusTickPacket full = tick;
    full.overheal = 0;
    decoder::apply_overheal(full, 100000, 100000);
    TEST_ASSERT_EQ(full.overheal, 20000u);

    // Unknown HP or a DoT tick is left alone.
    hub::ipc::StatusTickPacket unknown = tick;
    unknown.overheal = 0;
    decoder::apply_overheal(unknown, 0, 0);
    TEST_ASSERT_EQ(unknown.overheal, 0u);

    hub::ipc::StatusTickPacket dot{};
    dot.effect_type = static_cast<uint8_t>(EffectType::Damage);
    dot.damage_or_heal = 20000;
    decoder::apply_overheal(dot, 95000, 100000);
    TEST_ASSERT_EQ(dot.overheal, 0u);
}

TEST_CASE(MeterPlugin, HealsAreSplitIntoEffectiveAndOverhealBeforeRecording) {
    hub::ipc::PacketRingBuffer ring;
    CombatPlugin plugin;
    plugin.initialize();
    plugin.set_ring_buffer(&ring);
    plugin.set_hp_resolver([](uint32_t entity_id, uint32_t& current_hp, uint32_t& max_hp) {
        if (entity_id != 100) return false;
        current_hp = 95000;
        max_hp = 100000;
        return true;
    });

    // Open the pull with a hit, then heal a player who is only 5k down.
    hub::game::ActionEffectHeader hit{};
    hit.animation_target_id = 0x40001;
    hit.action_id = 31;
    hit.num_targets = 1;
    std::array<hub::game::ActionEffectEntry, 8> hit_entries{};
    hit_entries[0].effect_type = 0x03;
    hit_entries[0].value = 25000;
    plugin.on_receive_action_effect(777, nullptr, &hit, hit_entries.data(), nullptr);

    hub::game::ActionEffectHeader cure{};
    cure.animation_target_id = 100;
    cure.action_id = 135;
    cure.num_targets = 1;
    std::array<hub::game::ActionEffectEntry, 8> heal_entries{};
    heal_entries[0].effect_type = 0x04;
    heal_entries[0].value = 20000;
    plugin.on_receive_action_effect(777, nullptr, &cure, heal_entries.data(), nullptr);

    const auto summary = plugin.engine().current_summary();
    TEST_ASSERT_EQ(summary.total_effective_healing, 5000u);
    TEST_ASSERT_EQ(summary.total_overhealing, 15000u);
    TEST_ASSERT_EQ(summary.total_healing, 20000u);

    // The app's engine gets the same split, not the raw heal.
    std::vector<uint8_t> frame;
    bool saw_heal = false;
    while (ring.pop(frame)) {
        const auto header = hub::ipc::deserialize_header(frame);
        if (!header || header->message_type != static_cast<uint16_t>(hub::MessageType::CombatAction)) continue;
        hub::ipc::CombatActionPacket pkt{};
        std::memcpy(&pkt, frame.data() + sizeof(hub::ipc::PacketHeader), sizeof(pkt));
        if (pkt.effect_type != static_cast<uint16_t>(EffectType::Heal)) continue;
        saw_heal = true;
        TEST_ASSERT_EQ(pkt.effective_heal, 5000u);
        TEST_ASSERT_EQ(pkt.overheal, 15000u);
    }
    TEST_ASSERT_TRUE(saw_heal);
    plugin.shutdown();
}

TEST_CASE(MeterPlugin, HealTicksAreSplitBeforeRecording) {
    // Every HoT tick used to count as fully effective, whatever the target's HP.
    hub::ipc::PacketRingBuffer ring;
    CombatPlugin plugin;
    plugin.initialize();
    plugin.set_ring_buffer(&ring);
    plugin.set_hp_resolver([](uint32_t entity_id, uint32_t& current_hp, uint32_t& max_hp) {
        if (entity_id != 100) return false;
        current_hp = 95000;
        max_hp = 100000;
        return true;
    });

    // Open the pull with a hit, then a Regen tick on a player only 5k down.
    hub::game::ActionEffectHeader hit{};
    hit.animation_target_id = 0x40001;
    hit.action_id = 31;
    hit.num_targets = 1;
    std::array<hub::game::ActionEffectEntry, 8> hit_entries{};
    hit_entries[0].effect_type = 0x03;
    hit_entries[0].value = 25000;
    plugin.on_receive_action_effect(777, nullptr, &hit, hit_entries.data(), nullptr);

    plugin.on_status_tick(100, 777, 158, 20000, /*is_heal=*/true);

    const auto summary = plugin.engine().current_summary();
    TEST_ASSERT_EQ(summary.total_effective_healing, 5000u);
    TEST_ASSERT_EQ(summary.total_overhealing, 15000u);
    TEST_ASSERT_EQ(summary.total_healing, 20000u);

    // The app's engine gets the same split.
    std::vector<uint8_t> frame;
    bool saw_tick = false;
    while (ring.pop(frame)) {
        const auto header = hub::ipc::deserialize_header(frame);
        if (!header || header->message_type != static_cast<uint16_t>(hub::MessageType::CombatStatusTick)) continue;
        hub::ipc::StatusTickPacket pkt{};
        std::memcpy(&pkt, frame.data() + sizeof(hub::ipc::PacketHeader), sizeof(pkt));
        saw_tick = true;
        TEST_ASSERT_EQ(pkt.damage_or_heal, 20000u);
        TEST_ASSERT_EQ(pkt.overheal, 15000u);
    }
    TEST_ASSERT_TRUE(saw_tick);
    plugin.shutdown();
}

namespace {

hub::ipc::ActorInfoPacket party_actor(uint32_t id, Job job, uint32_t current_hp, uint32_t max_hp) {
    hub::ipc::ActorInfoPacket actor{};
    actor.entity_id = id;
    actor.job_id = static_cast<uint32_t>(job);
    actor.current_hp = current_hp;
    actor.max_hp = max_hp;
    actor.actor_type = static_cast<uint8_t>(ActorType::Player);
    return actor;
}

} // namespace

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

TEST_CASE(MeterRegistry, UnreadHpIsNotDeath) {
    // A party slot seen before its HP was ever read (max_hp 0) must not count as
    // dead: an all-unread party would otherwise look wiped and block every pull.
    CombatantRegistry reg;
    hub::ipc::PartySyncPacket party{};
    party.party_count = 2;
    party.entity_ids[0] = 1001; party.job_ids[0] = static_cast<uint32_t>(Job::WAR);
    party.entity_ids[1] = 1002; party.job_ids[1] = static_cast<uint32_t>(Job::WHM);
    reg.sync_party(party);
    TEST_ASSERT_FALSE(reg.is_party_wiped());

    reg.register_actor(party_actor(1001, Job::WAR, 0, 0));
    reg.register_actor(party_actor(1002, Job::WHM, 0, 0));
    TEST_ASSERT_FALSE(reg.is_party_wiped());

    // A 0 of a known max is a death, and is kept as one.
    reg.register_actor(1001, "War", Job::WAR, 0, ActorType::Player, 80000, 0);
    reg.register_actor(1002, "Whm", Job::WHM, 0, ActorType::Player, 60000, 0);
    TEST_ASSERT_TRUE(reg.is_party_wiped());
}

TEST_CASE(MeterRegistry, SoloWipeIsTheLocalPlayerAlone) {
    // With no party list the fallback counted every player and pet ever seen, and
    // one of them last read alive kept a solo death from ever being a wipe.
    CombatantRegistry reg;
    hub::ipc::PartySyncPacket solo{};
    solo.local_player_id = 1001;
    reg.sync_party(solo);
    reg.register_actor(1001, "Me", Job::WAR, 0, ActorType::Player, 80000, 80000);
    reg.register_actor(1002, "Former Member", Job::WHM, 0, ActorType::Player, 60000, 60000);
    reg.register_actor(1003, "Eos", Job::None, 0, ActorType::Pet, 5000, 5000);
    TEST_ASSERT_FALSE(reg.is_party_wiped());

    reg.update_hp(1001, 0);
    TEST_ASSERT_TRUE(reg.is_party_wiped());

    // Without a local player id the old count stands, but a pet is never a player.
    CombatantRegistry unknown;
    unknown.register_actor(2001, "Eos", Job::None, 0, ActorType::Pet, 5000, 0);
    TEST_ASSERT_FALSE(unknown.is_party_wiped());
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

TEST_CASE(MeterPlugin, DeathAndRaiseAreRepublished) {
    // HP stays out of the dedupe so it does not resend every tick, but the
    // alive/dead bit is what the app's wipe detection runs on.
    hub::ipc::PacketRingBuffer ring;
    hub::payload::ObjectReader reader(&ring);

    hub::game::CharacterObject chr{};
    chr.entity_id = 1001;
    chr.object_kind = 1;
    chr.class_job = static_cast<uint8_t>(Job::WAR);
    chr.max_hp = 80000;
    chr.current_hp = 80000;

    reader.inspect_and_sync_actor_direct(&chr);
    TEST_ASSERT_EQ(drain_actor_info(ring).size(), 1u);

    chr.current_hp = 40000;
    reader.inspect_and_sync_actor_direct(&chr);
    TEST_ASSERT_EQ(drain_actor_info(ring).size(), 0u);

    chr.current_hp = 0;
    reader.inspect_and_sync_actor_direct(&chr);
    auto published = drain_actor_info(ring);
    TEST_ASSERT_EQ(published.size(), 1u);
    TEST_ASSERT_EQ(published[0].current_hp, 0u);

    chr.current_hp = 20000;
    reader.inspect_and_sync_actor_direct(&chr);
    published = drain_actor_info(ring);
    TEST_ASSERT_EQ(published.size(), 1u);
    TEST_ASSERT_EQ(published[0].current_hp, 20000u);
}

TEST_CASE(MeterAccumulator, LateLinkedKnownPetIsNotDoubleCountedAsPetDamage) {
    MetricsAccumulator acc;
    CombatantRegistry reg;
    reg.register_actor(10, "Scholar", Job::SCH);
    reg.register_actor(30, "Eos"); // Known pet by name, owner not linked yet
    TEST_ASSERT_TRUE(reg.is_pet(30));
    TEST_ASSERT_EQ(reg.resolve_owner(30), 30u);

    hub::ipc::CombatActionPacket pet_hit{};
    pet_hit.source_id = 30;
    pet_hit.target_id = 0x40000001;
    pet_hit.damage = 4000;
    pet_hit.effect_type = static_cast<uint16_t>(EffectType::Damage);
    acc.record_action(pet_hit, reg);

    hub::ipc::CombatActionPacket own_hit = pet_hit;
    own_hit.source_id = 10;
    own_hit.damage = 6000;
    acc.record_action(own_hit, reg);

    reg.set_pet_owner(30, 10);
    acc.recalculate(5.0, &reg);

    const auto* sch = acc.find_stats(10);
    TEST_ASSERT(sch != nullptr);
    TEST_ASSERT_EQ(sch->total_damage, 10000u);
    TEST_ASSERT_EQ(sch->pet_damage, 4000u);
}

TEST_CASE(MeterAccumulator, PetMergedIntoOwnerWithoutRowTakesOwnersIdentity) {
    MetricsAccumulator acc;
    CombatantRegistry reg;
    reg.register_actor(10, "Scholar", Job::SCH);
    reg.register_actor(30, "Eos");

    hub::ipc::CombatActionPacket pet_hit{};
    pet_hit.source_id = 30;
    pet_hit.target_id = 0x40000001;
    pet_hit.damage = 4000;
    pet_hit.effect_type = static_cast<uint16_t>(EffectType::Damage);
    acc.record_action(pet_hit, reg);
    TEST_ASSERT(acc.find_stats(10) == nullptr);

    reg.set_pet_owner(30, 10);
    acc.recalculate(5.0, &reg);

    const auto* sch = acc.find_stats(10);
    TEST_ASSERT(sch != nullptr);
    TEST_ASSERT_EQ(sch->name, std::string("Scholar"));
    TEST_ASSERT(sch->actor_type == ActorType::Player);
    TEST_ASSERT_FALSE(sch->is_pet);
    TEST_ASSERT_EQ(sch->total_damage, 4000u);
    TEST_ASSERT_EQ(sch->pet_damage, 4000u);
    TEST_ASSERT(acc.find_stats(30) == nullptr);
}
