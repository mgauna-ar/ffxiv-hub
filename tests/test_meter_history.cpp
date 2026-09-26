#include "test_framework.hpp"
#include "meter_test_support.hpp"
#include "meter/types.hpp"
#include "meter/encounter_engine.hpp"
#include "meter/pull_grouping.hpp"
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

using namespace hub::meter;
using namespace hub::test::meter_support;

namespace {

PullHistoryEntry archived_entry(uint32_t zone, uint32_t visit) {
    PullHistoryEntry entry{};
    entry.zone_id = zone;
    entry.zone_visit = visit;
    return entry;
}

} // namespace

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
