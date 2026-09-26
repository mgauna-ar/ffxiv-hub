#include "test_framework.hpp"
#include "meter_test_support.hpp"
#include "meter/types.hpp"
#include "hub/game/entity.hpp"
#include "hub/game/pets.hpp"
#include "meter/combatant_registry.hpp"
#include "meter/metrics_accumulator.hpp"
#include <algorithm>

using namespace hub::meter;
using namespace hub::test::meter_support;

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
