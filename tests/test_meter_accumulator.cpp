#include "test_framework.hpp"
#include "meter/types.hpp"
#include "hub/game/entity.hpp"
#include "meter/combatant_registry.hpp"
#include "meter/metrics_accumulator.hpp"
#include <cstring>
#include <string>

using namespace hub::meter;

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
