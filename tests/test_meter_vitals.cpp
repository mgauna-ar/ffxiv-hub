#include "test_framework.hpp"
#include "meter/types.hpp"
#include "meter/combatant_registry.hpp"
#include "meter/metrics_accumulator.hpp"
#include "meter/encounter_engine.hpp"
#include "meter/combat_plugin.hpp"
#include "meter/vitals.hpp"
#include "hub/game/entity.hpp"
#include "hub/game/status.hpp"
#include "common/config/json.hpp"
#include "payload/object_reader.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <initializer_list>
#include <vector>

using namespace hub::meter;

namespace {

constexpr EntityId kTank = 0x10000001;
constexpr EntityId kHealer = 0x10000002;
constexpr EntityId kBoss = 0x40000001;

constexpr uint16_t kVulnerabilityUp = 638;  // Debuff
constexpr uint16_t kBattleLitany = 786;     // Buff
constexpr uint16_t kDia = 1871;             // Debuff (DoT)

constexpr uint64_t sec(double s) { return static_cast<uint64_t>(s * 1e6); }

hub::ipc::CombatStatusEntry status(uint16_t id, float remaining_s, uint32_t source, uint16_t param = 0) {
    hub::ipc::CombatStatusEntry entry{};
    entry.status_id = id;
    entry.param = param;
    entry.remaining_s = remaining_s;
    entry.source_id = source;
    return entry;
}

hub::ipc::StatusListPacket status_list(EntityId entity, uint64_t ts_us,
                                       std::initializer_list<hub::ipc::CombatStatusEntry> entries,
                                       uint8_t flags = 0) {
    hub::ipc::StatusListPacket list{};
    list.entity_id = entity;
    list.timestamp_us = ts_us;
    list.flags = flags;
    for (const auto& entry : entries) list.entries[list.count++] = entry;
    return list;
}

hub::ipc::CombatActionPacket hit(EntityId source, EntityId target, uint32_t amount, uint32_t action_id,
                                 uint64_t ts_us, EffectType effect = EffectType::Damage) {
    hub::ipc::CombatActionPacket packet{};
    packet.source_id = source;
    packet.target_id = target;
    packet.action_id = action_id;
    packet.damage = amount;
    packet.effect_type = static_cast<uint16_t>(effect);
    packet.timestamp_us = ts_us;
    return packet;
}

void sync_party(EncounterEngine& engine, std::initializer_list<std::pair<EntityId, Job>> members) {
    hub::ipc::PartySyncPacket sync{};
    for (const auto& [id, job] : members) {
        sync.entity_ids[sync.party_count] = id;
        sync.job_ids[sync.party_count] = static_cast<uint32_t>(job);
        ++sync.party_count;
    }
    engine.process_party_sync(sync);
}

hub::ipc::ActorInfoPacket actor_info(EntityId id, const char* name, Job job, uint32_t hp, uint32_t max_hp) {
    hub::ipc::ActorInfoPacket info{};
    info.entity_id = id;
    info.job_id = static_cast<uint32_t>(job);
    info.current_hp = hp;
    info.max_hp = max_hp;
    info.actor_type = static_cast<uint8_t>(ActorType::Player);
    std::strncpy(info.name, name, sizeof(info.name) - 1);
    return info;
}

const StatusUptimeRow* find_row(const EncounterSummary& summary, EntityId target, uint16_t status_id) {
    for (const auto& row : summary.statuses) {
        if (row.target == target && row.status == status_id) return &row;
    }
    return nullptr;
}

const StatusChange* find_change(const std::vector<StatusChange>& changes, uint16_t status_id, bool gained) {
    for (const auto& change : changes) {
        if (change.status_id == status_id && change.gained == gained) return &change;
    }
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// Registry status lists
// ---------------------------------------------------------------------------

TEST_CASE(MeterStatus, GainsAndRefreshes) {
    CombatantRegistry registry;
    std::vector<StatusChange> changes;

    registry.apply_status_list(status_list(kTank, sec(1), {status(kVulnerabilityUp, 10.0f, kBoss)}), changes);
    TEST_ASSERT_EQ(changes.size(), 1u);
    TEST_ASSERT(changes[0].gained);
    TEST_ASSERT_EQ(changes[0].at_us, sec(1));
    TEST_ASSERT_EQ(changes[0].source, kBoss);

    // A refresh moves the timer and is not a new application.
    changes.clear();
    registry.apply_status_list(status_list(kTank, sec(3), {status(kVulnerabilityUp, 10.0f, kBoss)}), changes);
    TEST_ASSERT(changes.empty());
    const Combatant* tank = registry.find_actor(kTank);
    TEST_ASSERT(tank != nullptr);
    TEST_ASSERT_EQ(tank->statuses.size(), 1u);
    TEST_ASSERT_EQ(tank->statuses[0].since_us, sec(1));
    TEST_ASSERT_EQ(tank->statuses[0].expected_end_us, sec(13));
}

TEST_CASE(MeterStatus, LossTimeFollowsTheTimer) {
    CombatantRegistry registry;
    std::vector<StatusChange> changes;

    // Ran out at 4 s, seen gone at 5 s: ended at 4 s.
    registry.apply_status_list(status_list(kTank, sec(1), {status(kVulnerabilityUp, 3.0f, kBoss),
                                                           status(kBattleLitany, 30.0f, kHealer)}), changes);
    changes.clear();
    registry.apply_status_list(status_list(kTank, sec(5), {status(kBattleLitany, 26.0f, kHealer)}), changes);
    const StatusChange* expired = find_change(changes, kVulnerabilityUp, false);
    TEST_ASSERT(expired != nullptr);
    TEST_ASSERT_EQ(expired->at_us, sec(4));

    // Removed long before its timer: ended when the list was read.
    changes.clear();
    registry.apply_status_list(status_list(kTank, sec(6), {}), changes);
    const StatusChange* removed = find_change(changes, kBattleLitany, false);
    TEST_ASSERT(removed != nullptr);
    TEST_ASSERT_EQ(removed->at_us, sec(6));
    TEST_ASSERT(registry.find_actor(kTank)->statuses.empty());
}

TEST_CASE(MeterStatus, StatusWithoutTimerEndsWhenRead) {
    CombatantRegistry registry;
    std::vector<StatusChange> changes;
    registry.apply_status_list(status_list(kTank, sec(1), {status(kBattleLitany, 0.0f, kTank)}), changes);
    changes.clear();
    registry.apply_status_list(status_list(kTank, sec(9), {}), changes);
    TEST_ASSERT_EQ(changes.size(), 1u);
    TEST_ASSERT_EQ(changes[0].at_us, sec(9));
}

TEST_CASE(MeterStatus, ListWithoutDetailContinuesTheStatus) {
    // A member walking out of range switches to the party list's copy, which has
    // no sources: the same status must carry on, not restart.
    CombatantRegistry registry;
    std::vector<StatusChange> changes;
    registry.apply_status_list(status_list(kTank, sec(1), {status(kVulnerabilityUp, 10.0f, kBoss)}), changes);
    changes.clear();
    registry.apply_status_list(status_list(kTank, sec(2), {status(kVulnerabilityUp, 0.0f, 0)},
                                           hub::ipc::STATUS_LIST_NO_DETAIL), changes);
    TEST_ASSERT(changes.empty());
    TEST_ASSERT_EQ(registry.find_actor(kTank)->statuses[0].source, kBoss);
    TEST_ASSERT_EQ(registry.find_actor(kTank)->statuses[0].expected_end_us, 0u);
}

TEST_CASE(MeterStatus, EmptyListForUnknownActorCreatesNothing) {
    CombatantRegistry registry;
    std::vector<StatusChange> changes;
    registry.apply_status_list(status_list(kTank, sec(1), {}), changes);
    TEST_ASSERT(registry.find_actor(kTank) == nullptr);
    TEST_ASSERT(changes.empty());
}

TEST_CASE(MeterStatus, DetrimentalTableMatchesTheSheet) {
    TEST_ASSERT(hub::game::status_is_detrimental(kVulnerabilityUp));
    TEST_ASSERT(hub::game::status_is_detrimental(kDia));
    TEST_ASSERT(!hub::game::status_is_detrimental(kBattleLitany));
    TEST_ASSERT(!hub::game::status_is_detrimental(0));
    TEST_ASSERT(!hub::game::status_is_detrimental(70000));
}

// ---------------------------------------------------------------------------
// Uptime over a pull
// ---------------------------------------------------------------------------

TEST_CASE(MeterStatus, PrepullBuffCountsFromTheStart) {
    EncounterEngine engine(7.0);
    sync_party(engine, {{kTank, Job::WAR}, {kHealer, Job::WHM}});
    const auto t0 = std::chrono::steady_clock::now();

    engine.process_status_list(status_list(kTank, sec(0.5), {status(kBattleLitany, 0.0f, kHealer)}));
    engine.process_action(hit(kTank, kBoss, 1000, 31, sec(1)), t0);
    engine.process_action(hit(kTank, kBoss, 1000, 31, sec(6)), t0 + std::chrono::seconds(5));
    engine.process_action(hit(kTank, kBoss, 1000, 31, sec(11)), t0 + std::chrono::seconds(10));
    engine.update(t0 + std::chrono::seconds(18));
    TEST_ASSERT_EQ(engine.state(), EncounterState::Complete);

    const auto pull = engine.latest_pull();
    TEST_ASSERT(pull.has_value());
    const StatusUptimeRow* row = find_row(*pull, kTank, kBattleLitany);
    TEST_ASSERT(row != nullptr);
    TEST_ASSERT_EQ(row->applications, 1u);
    TEST_ASSERT_NEAR(row->active_s, 10.0, 0.01);
    TEST_ASSERT_NEAR(row->uptime_pct, 100.0, 0.01);
    TEST_ASSERT(!row->detrimental);
    TEST_ASSERT(!row->on_enemy);
}

TEST_CASE(MeterStatus, TrimmedEndClampsUptime) {
    EncounterEngine engine(7.0);
    sync_party(engine, {{kTank, Job::WAR}});
    const auto t0 = std::chrono::steady_clock::now();

    engine.process_action(hit(kTank, kBoss, 1000, 31, sec(1)), t0);
    engine.process_status_list(status_list(kTank, sec(6), {status(kVulnerabilityUp, 30.0f, kBoss)}));
    engine.process_action(hit(kTank, kBoss, 1000, 31, sec(6)), t0 + std::chrono::seconds(5));
    engine.process_action(hit(kTank, kBoss, 1000, 31, sec(11)), t0 + std::chrono::seconds(10));
    // Removed after the last activity, before the inactivity end was noticed.
    engine.process_status_list(status_list(kTank, sec(14), {}));
    engine.update(t0 + std::chrono::seconds(18));

    const auto pull = engine.latest_pull();
    TEST_ASSERT(pull.has_value());
    const StatusUptimeRow* row = find_row(*pull, kTank, kVulnerabilityUp);
    TEST_ASSERT(row != nullptr);
    TEST_ASSERT(row->detrimental);
    TEST_ASSERT_NEAR(row->active_s, 5.0, 0.01);
    TEST_ASSERT_NEAR(row->uptime_pct, 50.0, 0.1);
}

TEST_CASE(MeterStatus, EnemyStatusesAreClearedAtPullEnd) {
    EncounterEngine engine(7.0);
    sync_party(engine, {{kTank, Job::WHM}});
    const auto t0 = std::chrono::steady_clock::now();

    engine.process_action(hit(kTank, kBoss, 1000, 31, sec(1)), t0);
    engine.process_status_list(status_list(kBoss, sec(2), {status(kDia, 30.0f, kTank)}));
    engine.process_action(hit(kTank, kBoss, 1000, 31, sec(5)), t0 + std::chrono::seconds(4));
    engine.update(t0 + std::chrono::seconds(12));

    const auto pull = engine.latest_pull();
    TEST_ASSERT(pull.has_value());
    const StatusUptimeRow* dia = find_row(*pull, kBoss, kDia);
    TEST_ASSERT(dia != nullptr);
    TEST_ASSERT(dia->on_enemy);
    TEST_ASSERT_EQ(dia->source, kTank);
    // Up from 2 s until the trimmed end at 5 s.
    TEST_ASSERT_NEAR(dia->active_s, 3.0, 0.01);

    // Nothing refreshes an enemy's list once it is no longer tracked.
    const bool cleared = engine.with_registry([](CombatantRegistry& registry) {
        const Combatant* boss = registry.find_actor(kBoss);
        return boss == nullptr || boss->statuses.empty();
    });
    TEST_ASSERT(cleared);

    engine.process_action(hit(kTank, kBoss, 1000, 31, sec(30)), t0 + std::chrono::seconds(29));
    TEST_ASSERT(find_row(engine.current_summary(t0 + std::chrono::seconds(30)), kBoss, kDia) == nullptr);
}

TEST_CASE(MeterStatus, ListsNeverStartAPullOrKeepItAlive) {
    EncounterEngine engine(7.0);
    sync_party(engine, {{kTank, Job::WAR}});
    const auto t0 = std::chrono::steady_clock::now();

    engine.process_status_list(status_list(kTank, sec(1), {status(kBattleLitany, 10.0f, kTank)}));
    hub::ipc::LifeEventPacket death{};
    death.entity_id = kTank;
    death.kind = static_cast<uint8_t>(LifeEventKind::Death);
    death.timestamp_us = sec(1);
    engine.process_life_event(death);
    TEST_ASSERT_EQ(engine.state(), EncounterState::Idle);
    TEST_ASSERT(engine.pull_history_index().empty());

    engine.process_action(hit(kTank, kBoss, 1000, 31, sec(2)), t0);
    for (int i = 1; i <= 8; ++i) {
        engine.process_status_list(status_list(kTank, sec(2 + i), {status(kBattleLitany, 10.0f - i, kTank)}));
    }
    engine.update(t0 + std::chrono::milliseconds(7500));
    TEST_ASSERT_EQ(engine.state(), EncounterState::Complete);
}

// ---------------------------------------------------------------------------
// Damage taken, recaps and deaths
// ---------------------------------------------------------------------------

TEST_CASE(MeterDeaths, DamageTakenRowsPerAbilityAndSource) {
    EncounterEngine engine;
    sync_party(engine, {{kTank, Job::WAR}});
    const auto t0 = std::chrono::steady_clock::now();

    engine.process_action(hit(kTank, kBoss, 1000, 31, sec(1)), t0);
    engine.process_action(hit(kBoss, kTank, 5000, 1000, sec(2)), t0);
    engine.process_action(hit(kBoss, kTank, 7000, 1000, sec(3)), t0);

    hub::ipc::StatusTickPacket tick{};
    tick.target_id = kTank;
    tick.source_id = kBoss;
    tick.status_id = 2941;
    tick.damage_or_heal = 300;
    tick.effect_type = static_cast<uint8_t>(EffectType::Damage);
    tick.timestamp_us = sec(4);
    engine.process_status_tick(tick, t0);

    const auto summary = engine.current_summary(t0 + std::chrono::seconds(4));
    TEST_ASSERT_EQ(summary.damage_taken.size(), 2u);
    const DamageTakenRow& first = summary.damage_taken[0];
    TEST_ASSERT_EQ(first.target, kTank);
    TEST_ASSERT_EQ(first.action_key, 1000u);
    TEST_ASSERT_EQ(first.source, kBoss);
    TEST_ASSERT_EQ(first.hits, 2u);
    TEST_ASSERT_EQ(first.total, 12000u);
    TEST_ASSERT_EQ(first.max, 7000u);
    TEST_ASSERT_EQ(summary.damage_taken[1].action_key, 2941u | STATUS_ACTION_KEY_OFFSET);

    // The boss is not a friendly target, so the party's hits on it are not "taken".
    for (const auto& row : summary.damage_taken) TEST_ASSERT_EQ(row.target, kTank);
}

TEST_CASE(MeterDeaths, RecapKeepsTheNewestInsideTheWindow) {
    EncounterEngine engine;
    sync_party(engine, {{kTank, Job::WAR}});
    const auto t0 = std::chrono::steady_clock::now();

    engine.process_action(hit(kTank, kBoss, 1000, 31, sec(0.5)), t0);
    engine.process_action(hit(kBoss, kTank, 100, 1000, sec(0.8)), t0);   // Outside the window
    for (int i = 1; i <= 12; ++i) {
        engine.process_action(hit(kBoss, kTank, 1000 + i, 2000 + i, sec(12 + i)), t0);
    }
    const auto event = engine.build_life_event(kTank, LifeEventKind::Death, sec(24.5));
    TEST_ASSERT_EQ(event.recap_count, hub::ipc::MAX_LIFE_EVENT_RECAP);
    // Oldest first, and only the newest ten.
    TEST_ASSERT_EQ(event.recap[0].action_key, 2003u);
    TEST_ASSERT_EQ(event.recap[9].action_key, 2012u);
    TEST_ASSERT_EQ(event.recap[9].offset_ms, -500);
    TEST_ASSERT_EQ(event.recap[0].offset_ms, -9500);

    const auto raise = engine.build_life_event(kTank, LifeEventKind::Raise, sec(30));
    TEST_ASSERT_EQ(raise.recap_count, 0u);
    // Enemies keep no recap.
    TEST_ASSERT_EQ(engine.build_life_event(kBoss, LifeEventKind::Death, sec(24.5)).recap_count, 0u);
}

TEST_CASE(MeterDeaths, DeathRecordsKillingBlowStatusesAndRaise) {
    EncounterEngine engine;
    sync_party(engine, {{kTank, Job::WAR}, {kHealer, Job::WHM}});
    const auto t0 = std::chrono::steady_clock::now();

    engine.process_action(hit(kTank, kBoss, 1000, 31, sec(1)), t0);
    engine.process_action(hit(kBoss, kTank, 20000, 1000, sec(3)), t0);
    engine.process_action(hit(kHealer, kTank, 5000, 124, sec(3.5), EffectType::Heal), t0);
    engine.process_action(hit(kBoss, kTank, 30000, 1001, sec(4)), t0);
    engine.process_status_list(status_list(kTank, sec(3.8), {status(kBattleLitany, 10.0f, kHealer),
                                                             status(kVulnerabilityUp, 10.0f, kBoss)}));

    engine.process_life_event(engine.build_life_event(kTank, LifeEventKind::Death, sec(4.1)));
    hub::ipc::LifeEventPacket raise{};
    raise.entity_id = kTank;
    raise.kind = static_cast<uint8_t>(LifeEventKind::Raise);
    raise.timestamp_us = sec(10.1);
    engine.process_life_event(raise);

    const auto summary = engine.current_summary(t0 + std::chrono::seconds(10));
    TEST_ASSERT_EQ(summary.deaths.size(), 1u);
    const DeathRecord& death = summary.deaths[0];
    TEST_ASSERT_EQ(death.entity, kTank);
    TEST_ASSERT_NEAR(death.time_s, 3.1, 0.001);
    TEST_ASSERT_EQ(death.recap_count, 3u);
    TEST_ASSERT(death.killing_blow >= 0);
    TEST_ASSERT_EQ(death.recap[death.killing_blow].action_key, 1001u);
    TEST_ASSERT(death.recap[1].kind == RecapKind::Heal);
    // Debuffs first.
    TEST_ASSERT_EQ(death.status_count, 2u);
    TEST_ASSERT_EQ(death.statuses[0], kVulnerabilityUp);
    TEST_ASSERT_EQ(death.statuses[1], kBattleLitany);
    TEST_ASSERT_NEAR(death.raised_after_s, 6.0, 0.001);

    const auto tank = std::find_if(summary.combatants.begin(), summary.combatants.end(),
                                   [](const CombatantStats& c) { return c.entity_id == kTank; });
    TEST_ASSERT(tank != summary.combatants.end());
    TEST_ASSERT_EQ(tank->deaths, 1u);
    TEST_ASSERT_EQ(tank->raises, 1u);

    const auto blow = std::find_if(summary.damage_taken.begin(), summary.damage_taken.end(),
                                   [](const DamageTakenRow& r) { return r.action_key == 1001u; });
    TEST_ASSERT(blow != summary.damage_taken.end());
    TEST_ASSERT_EQ(blow->deaths_caused, 1u);
}

TEST_CASE(MeterDeaths, LateDeathJoinsTheWipedPull) {
    // The wipe can be detected from actor info on one lane before the deaths
    // arrive on another; they still belong to that pull.
    EncounterEngine engine;
    sync_party(engine, {{kTank, Job::WAR}, {kHealer, Job::WHM}});
    engine.process_actor_info(actor_info(kTank, "Tank", Job::WAR, 100, 100));
    engine.process_actor_info(actor_info(kHealer, "Healer", Job::WHM, 100, 100));
    const auto t0 = std::chrono::steady_clock::now();

    engine.process_action(hit(kTank, kBoss, 1000, 31, sec(1)), t0);
    engine.process_action(hit(kBoss, kTank, 99999, 1000, sec(5)), t0 + std::chrono::seconds(4));
    engine.process_action(hit(kBoss, kHealer, 99999, 1000, sec(5)), t0 + std::chrono::seconds(4));
    engine.process_actor_info(actor_info(kTank, "Tank", Job::WAR, 0, 100));
    engine.process_actor_info(actor_info(kHealer, "Healer", Job::WHM, 0, 100));
    TEST_ASSERT_EQ(engine.state(), EncounterState::Wipe);
    const uint64_t revision = engine.history_revision();

    hub::ipc::LifeEventPacket death{};
    death.kind = static_cast<uint8_t>(LifeEventKind::Death);
    death.entity_id = kTank;
    death.timestamp_us = sec(5.2);
    engine.process_life_event(death);
    death.entity_id = kHealer;
    engine.process_life_event(death);
    // The pull list re-reads the archive on this, so its death count follows.
    const uint64_t joined = engine.history_revision();
    TEST_ASSERT_TRUE(joined != revision);
    // Too long after the pull to be part of it.
    death.timestamp_us = sec(5) + EncounterEngine::kLateLifeEventUs + sec(1);
    engine.process_life_event(death);
    TEST_ASSERT_EQ(engine.history_revision(), joined);

    const auto pull = engine.latest_pull();
    TEST_ASSERT(pull.has_value());
    TEST_ASSERT_EQ(pull->deaths.size(), 2u);
    TEST_ASSERT_EQ(engine.pull_history_index()[0].death_count, 2u);
    for (const auto& c : pull->combatants) {
        if (c.entity_id == kTank || c.entity_id == kHealer) TEST_ASSERT_EQ(c.deaths, 1u);
    }
    // The live view still shows that pull, so it agrees.
    TEST_ASSERT_EQ(engine.current_summary().deaths.size(), 2u);
}

TEST_CASE(MeterDeaths, RankingsSnapshotCarriesNoDetail) {
    EncounterEngine engine;
    sync_party(engine, {{kTank, Job::WAR}});
    const auto t0 = std::chrono::steady_clock::now();
    engine.process_action(hit(kTank, kBoss, 1000, 31, sec(1)), t0);
    engine.process_action(hit(kBoss, kTank, 5000, 1000, sec(2)), t0);
    engine.process_status_list(status_list(kTank, sec(2), {status(kVulnerabilityUp, 10.0f, kBoss)}));

    const auto full = engine.current_summary(t0 + std::chrono::seconds(2));
    TEST_ASSERT(!full.damage_taken.empty());
    TEST_ASSERT(!full.statuses.empty());

    const auto rankings = engine.current_rankings(t0 + std::chrono::seconds(2));
    TEST_ASSERT_EQ(rankings.combatants.size(), full.combatants.size());
    TEST_ASSERT_EQ(rankings.total_damage, full.total_damage);
    for (const auto& c : rankings.combatants) TEST_ASSERT(c.actions.empty());
    TEST_ASSERT(rankings.deaths.empty());
    TEST_ASSERT(rankings.damage_taken.empty());
    TEST_ASSERT(rankings.statuses.empty());
    TEST_ASSERT(rankings.names.empty());
}

// ---------------------------------------------------------------------------
// Vitals tracker and the plugin's pass
// ---------------------------------------------------------------------------

namespace {

ActorVitals party_vitals(EntityId id, uint32_t hp, std::initializer_list<hub::ipc::CombatStatusEntry> entries) {
    ActorVitals vitals{};
    vitals.entity = id;
    vitals.hp = hp;
    vitals.max_hp = 100;
    vitals.track_life = true;
    vitals.statuses_read = true;
    for (const auto& entry : entries) vitals.entries[vitals.count++] = entry;
    return vitals;
}

std::vector<hub::MessageType> drain_types(hub::ipc::PacketRingBuffer& ring,
                                          std::vector<hub::ipc::StatusListPacket>* lists = nullptr) {
    std::vector<hub::MessageType> types;
    std::vector<uint8_t> frame;
    while (ring.pop(frame)) {
        const auto header = hub::ipc::deserialize_header(frame);
        if (!header) continue;
        const auto type = static_cast<hub::MessageType>(header->message_type);
        types.push_back(type);
        if (lists && type == hub::MessageType::CombatStatusList) {
            hub::ipc::StatusListPacket list{};
            std::memcpy(&list, frame.data() + sizeof(hub::ipc::PacketHeader), sizeof(list));
            lists->push_back(list);
        }
    }
    return types;
}

} // namespace

TEST_CASE(MeterVitals, LifeFirstSightingIsSilent) {
    VitalsTracker tracker;
    ActorVitals vitals = party_vitals(kTank, 0, {});
    TEST_ASSERT(!tracker.observe_life(vitals).has_value());  // Already dead when first seen
    vitals.hp = 50;
    TEST_ASSERT(tracker.observe_life(vitals) == LifeEventKind::Raise);
    vitals.hp = 0;
    TEST_ASSERT(tracker.observe_life(vitals) == LifeEventKind::Death);
    TEST_ASSERT(!tracker.observe_life(vitals).has_value());
    vitals.max_hp = 0;  // Unread HP proves nothing
    vitals.hp = 0;
    TEST_ASSERT(!tracker.observe_life(vitals).has_value());
}

TEST_CASE(MeterVitals, ListResentOnlyWhenItChanged) {
    VitalsTracker tracker;
    TEST_ASSERT(tracker.status_list_changed(status_list(kTank, sec(0), {status(kVulnerabilityUp, 10.0f, kBoss)})));
    // Counting down as predicted.
    TEST_ASSERT(!tracker.status_list_changed(status_list(kTank, sec(1), {status(kVulnerabilityUp, 9.0f, kBoss)})));
    // Refreshed: back above its countdown.
    TEST_ASSERT(tracker.status_list_changed(status_list(kTank, sec(2), {status(kVulnerabilityUp, 10.0f, kBoss)})));
    // Stacks changed.
    TEST_ASSERT(tracker.status_list_changed(status_list(kTank, sec(2.5), {status(kVulnerabilityUp, 9.5f, kBoss, 2)})));
    TEST_ASSERT(!tracker.status_list_changed(status_list(kTank, sec(3), {status(kVulnerabilityUp, 9.0f, kBoss, 2)})));
    // A reconnect resends everything once.
    tracker.invalidate();
    TEST_ASSERT(tracker.status_list_changed(status_list(kTank, sec(3.2), {status(kVulnerabilityUp, 8.8f, kBoss, 2)})));
    TEST_ASSERT(!tracker.status_list_changed(status_list(kTank, sec(3.4), {status(kVulnerabilityUp, 8.6f, kBoss, 2)})));
}

TEST_CASE(MeterVitals, RetainClearsWhatLeft) {
    VitalsTracker tracker;
    TEST_ASSERT(tracker.status_list_changed(status_list(kTank, sec(0), {status(kVulnerabilityUp, 10.0f, kBoss)})));
    TEST_ASSERT(tracker.status_list_changed(status_list(kHealer, sec(0), {})));
    const std::vector<EntityId> seen{kHealer};
    const auto cleared = tracker.retain(seen);
    TEST_ASSERT_EQ(cleared.size(), 1u);
    TEST_ASSERT_EQ(cleared[0], kTank);
    TEST_ASSERT_EQ(tracker.tracked_count(), 1u);
    // An actor whose last list was empty needs no clearing.
    TEST_ASSERT(tracker.retain({}).empty());
    TEST_ASSERT_EQ(tracker.tracked_count(), 0u);
}

TEST_CASE(MeterVitals, PassSendsDeathBeforeItsStatusList) {
    CombatPlugin plugin;
    plugin.initialize();
    hub::ipc::PacketRingBuffer ring;
    plugin.set_ring_buffer(&ring);
    sync_party(plugin.engine(), {{kTank, Job::WAR}});
    const auto t0 = std::chrono::steady_clock::now();
    plugin.engine().process_action(hit(kTank, kBoss, 1000, 31, sec(1)), t0);

    std::vector<ActorVitals> pass{party_vitals(kTank, 100, {status(kVulnerabilityUp, 10.0f, kBoss)})};
    plugin.on_vitals(pass, sec(2));
    TEST_ASSERT((drain_types(ring) == std::vector<hub::MessageType>{hub::MessageType::CombatStatusList}));

    pass[0] = party_vitals(kTank, 0, {});
    plugin.on_vitals(pass, sec(3));
    TEST_ASSERT((drain_types(ring) == std::vector<hub::MessageType>{
        hub::MessageType::CombatLifeEvent, hub::MessageType::CombatStatusList}));

    // Recorded with the statuses held before the list that dropped them.
    const auto summary = plugin.engine().current_summary(t0 + std::chrono::seconds(2));
    TEST_ASSERT_EQ(summary.deaths.size(), 1u);
    TEST_ASSERT_EQ(summary.deaths[0].status_count, 1u);
    TEST_ASSERT_EQ(summary.deaths[0].statuses[0], kVulnerabilityUp);

    // Nothing changed: nothing sent.
    plugin.on_vitals(pass, sec(3.25));
    TEST_ASSERT(drain_types(ring).empty());
}

TEST_CASE(MeterVitals, EnemyListKeepsOnlyThePartysStatuses) {
    CombatPlugin plugin;
    plugin.initialize();
    hub::ipc::PacketRingBuffer ring;
    plugin.set_ring_buffer(&ring);
    sync_party(plugin.engine(), {{kTank, Job::WHM}});

    ActorVitals boss{};
    boss.entity = kBoss;
    boss.is_enemy = true;
    boss.statuses_read = true;
    boss.entries[boss.count++] = status(kDia, 20.0f, kTank);
    boss.entries[boss.count++] = status(kBattleLitany, 20.0f, kBoss);            // Its own
    boss.entries[boss.count++] = status(kVulnerabilityUp, 20.0f, hub::game::NO_ENTITY_ID);
    boss.entries[boss.count++] = status(65000, 20.0f, kTank);                     // Nameless
    std::vector<ActorVitals> pass{boss};

    std::vector<hub::ipc::StatusListPacket> lists;
    plugin.on_vitals(pass, sec(1));
    drain_types(ring, &lists);
    TEST_ASSERT_EQ(lists.size(), 1u);
    TEST_ASSERT_EQ(lists[0].count, 1u);
    TEST_ASSERT_EQ(lists[0].entries[0].status_id, kDia);

    // Switched off: every list sent so far is cleared once, then silence.
    plugin.set_vitals_tracking(false);
    TEST_ASSERT(!plugin.vitals_enabled());
    lists.clear();
    plugin.on_vitals({}, sec(2));
    drain_types(ring, &lists);
    TEST_ASSERT_EQ(lists.size(), 1u);
    TEST_ASSERT_EQ(lists[0].entity_id, kBoss);
    TEST_ASSERT_EQ(lists[0].count, 0u);
    plugin.on_vitals(pass, sec(3));
    TEST_ASSERT(drain_types(ring).empty());
}

TEST_CASE(MeterVitals, NewPullOnTheSameEnemyGetsItsListBack) {
    // Pull A ends (dropping the boss's list) and pull B starts on the same boss
    // before any pass runs in between: B must still see the DoT.
    CombatPlugin plugin;
    plugin.initialize();
    hub::ipc::PacketRingBuffer ring;
    plugin.set_ring_buffer(&ring);
    EncounterEngine& engine = plugin.engine();
    engine.set_inactivity_timeout(7.0);
    sync_party(engine, {{kTank, Job::WHM}});
    const auto t0 = std::chrono::steady_clock::now();

    ActorVitals boss{};
    boss.entity = kBoss;
    boss.is_enemy = true;
    boss.statuses_read = true;
    boss.entries[boss.count++] = status(kDia, 30.0f, kTank);
    std::vector<ActorVitals> pass{boss};

    engine.process_action(hit(kTank, kBoss, 1000, 31, sec(1)), t0);
    plugin.on_vitals(pass, sec(2));
    engine.process_action(hit(kTank, kBoss, 1000, 31, sec(3)), t0 + std::chrono::seconds(2));
    engine.update(t0 + std::chrono::seconds(10));
    TEST_ASSERT_EQ(engine.state(), EncounterState::Complete);
    engine.process_action(hit(kTank, kBoss, 1000, 31, sec(11)), t0 + std::chrono::seconds(10));
    TEST_ASSERT_EQ(engine.state(), EncounterState::InCombat);

    pass[0].entries[0].remaining_s = 21.0f;  // Counting down, unchanged
    plugin.on_vitals(pass, sec(11.2));
    const auto summary = engine.current_summary(t0 + std::chrono::seconds(14));
    TEST_ASSERT(find_row(summary, kBoss, kDia) != nullptr);
}

TEST_CASE(MeterVitals, TrackVitalsIsAPersistedSetting) {
    CombatPlugin plugin;
    plugin.initialize();
    TEST_ASSERT(plugin.vitals_enabled());

    hub::config::JsonValue in(hub::config::JsonValue::ObjectType{});
    in["track_vitals"] = hub::config::JsonValue(false);
    plugin.deserialize_config(in);
    TEST_ASSERT(!plugin.vitals_enabled());

    hub::config::JsonValue out(hub::config::JsonValue::ObjectType{});
    plugin.serialize_config(out);
    TEST_ASSERT(!out["track_vitals"].as_bool(true));

    // The whole plugin off means no polling either.
    plugin.set_vitals_tracking(true);
    plugin.set_enabled(false);
    TEST_ASSERT(!plugin.vitals_enabled());
}

// ---------------------------------------------------------------------------
// Reading the game's StatusManager
// ---------------------------------------------------------------------------

TEST_CASE(MeterVitals, ExtractStatusListChecksTheLayout) {
    hub::game::StatusManagerObject manager{};
    int owner = 0;
    int stranger = 0;
    manager.owner = &owner;
    manager.slot_count = 30;
    manager.statuses[0] = hub::game::StatusEntry{kVulnerabilityUp, 2, 12.5f, kBoss};
    manager.statuses[4] = hub::game::StatusEntry{kBattleLitany, 0, 20.0f, 0xABCD'0000'0000'0000ull | kHealer};
    manager.statuses[40] = hub::game::StatusEntry{kDia, 0, 5.0f, kTank};  // Beyond 30 slots

    ActorVitals vitals{};
    TEST_ASSERT(hub::payload::detail::extract_status_list(&manager, &owner, true, vitals));
    TEST_ASSERT(vitals.statuses_read);
    TEST_ASSERT_EQ(vitals.count, 2u);
    TEST_ASSERT_EQ(vitals.entries[0].status_id, kVulnerabilityUp);
    TEST_ASSERT_EQ(vitals.entries[0].param, 2u);
    TEST_ASSERT_NEAR(vitals.entries[0].remaining_s, 12.5f, 0.001f);
    TEST_ASSERT_EQ(vitals.entries[1].source_id, kHealer);  // Low 32 bits

    manager.slot_count = 60;
    TEST_ASSERT(hub::payload::detail::extract_status_list(&manager, &owner, true, vitals));
    TEST_ASSERT_EQ(vitals.count, 3u);

    // The party list's copy: no timers, no sources, and no owner to check.
    manager.owner = nullptr;
    TEST_ASSERT(hub::payload::detail::extract_status_list(&manager, nullptr, false, vitals));
    TEST_ASSERT(!vitals.status_detail);
    TEST_ASSERT_EQ(vitals.entries[0].remaining_s, 0.0f);
    TEST_ASSERT_EQ(vitals.entries[0].source_id, 0u);

    // A moved layout: another owner, or a slot count that is neither 30 nor 60.
    manager.owner = &stranger;
    TEST_ASSERT(!hub::payload::detail::extract_status_list(&manager, &owner, true, vitals));
    TEST_ASSERT(!vitals.statuses_read);
    manager.owner = &owner;
    manager.slot_count = 17;
    TEST_ASSERT(!hub::payload::detail::extract_status_list(&manager, &owner, true, vitals));
    TEST_ASSERT_EQ(vitals.count, 0u);
}

TEST_CASE(MeterVitals, WireStructSizes) {
    TEST_ASSERT_EQ(sizeof(hub::ipc::CombatStatusEntry), 12u);
    TEST_ASSERT_EQ(sizeof(hub::ipc::CombatStatusListPayload), 376u);
    TEST_ASSERT_EQ(sizeof(hub::ipc::CombatRecapEntry), 20u);
    TEST_ASSERT_EQ(sizeof(hub::ipc::CombatLifeEventPayload), 216u);
    TEST_ASSERT_EQ(sizeof(hub::game::StatusEntry), 16u);
}
