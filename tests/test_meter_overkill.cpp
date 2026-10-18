#include "test_framework.hpp"
#include "meter/combat_plugin.hpp"
#include "meter/encounter_engine.hpp"
#include "meter/metrics_accumulator.hpp"
#include "meter/overkill.hpp"
#include "meter/types.hpp"
#include "hub/game/limit_break.hpp"
#include "common/ipc/ring_buffer.hpp"
#include <array>
#include <cstring>
#include <vector>

using namespace hub::meter;

namespace {

constexpr EntityId kTank = 0x10000001;
constexpr EntityId kHealer = 0x10000002;
constexpr EntityId kBoss = 0x40000001;
constexpr uint32_t kMeteor = 205;  // A caster Limit Break 3

constexpr uint64_t ms(uint64_t m) { return m * 1000; }

void sync_party(EncounterEngine& engine) {
    hub::ipc::PartySyncPacket sync{};
    sync.entity_ids[0] = kTank;
    sync.job_ids[0] = static_cast<uint32_t>(Job::WAR);
    sync.entity_ids[1] = kHealer;
    sync.job_ids[1] = static_cast<uint32_t>(Job::WHM);
    sync.party_count = 2;
    engine.process_party_sync(sync);
}

hub::ipc::CombatActionPacket hit(EntityId source, EntityId target, uint32_t damage, uint32_t overkill) {
    hub::ipc::CombatActionPacket packet{};
    packet.source_id = source;
    packet.target_id = target;
    packet.action_id = 31;
    packet.damage = damage;
    packet.overkill = overkill;
    packet.effect_type = static_cast<uint16_t>(EffectType::Damage);
    packet.timestamp_us = ms(1000);
    return packet;
}

/// One effect of `amount` from `source` on `target`.
void effect(CombatPlugin& plugin, EntityId source, EntityId target, uint32_t action_id, uint32_t amount,
            uint8_t effect_type = 0x03) {
    hub::game::ActionEffectHeader header{};
    header.animation_target_id = target;
    header.action_id = action_id;
    header.num_targets = 1;
    std::array<hub::game::ActionEffectEntry, 8> entries{};
    entries[0].effect_type = effect_type;
    entries[0].value = static_cast<uint16_t>(amount);
    plugin.on_receive_action_effect(source, nullptr, &header, entries.data(), nullptr);
}

std::vector<hub::ipc::CombatActionPacket> drain_actions(hub::ipc::PacketRingBuffer& ring) {
    std::vector<hub::ipc::CombatActionPacket> sent;
    std::vector<uint8_t> frame;
    while (ring.pop(frame)) {
        const auto header = hub::ipc::deserialize_header(frame);
        if (!header || static_cast<hub::MessageType>(header->message_type) != hub::MessageType::CombatAction) continue;
        hub::ipc::CombatActionPacket packet{};
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

TEST_CASE(MeterOverkill, LedgerTakesHitsStillOnTheirWayOffTheHp) {
    OverkillLedger ledger;
    TEST_ASSERT_EQ(ledger.book(kBoss, 300, 1000, ms(0)), 0u);
    // The first hit has not applied yet: 700 is left, not 1000.
    TEST_ASSERT_EQ(ledger.book(kBoss, 900, 1000, ms(100)), 200u);
    // The first hit applied (HP 1000 -> 700); the second, 700 of it, has not.
    TEST_ASSERT_EQ(ledger.book(kBoss, 500, 700, ms(600)), 500u);
    // Both applied: the boss reads 0 and every hit on it now is overkill.
    TEST_ASSERT_EQ(ledger.book(kBoss, 400, 0, ms(1200)), 400u);
}

TEST_CASE(MeterOverkill, LedgerForgetsWhatTheHpWouldHaveShownBy) {
    OverkillLedger ledger;
    TEST_ASSERT_EQ(ledger.book(kBoss, 800, 1000, ms(0)), 0u);
    // Healed back up meanwhile: the HP read says nothing about the pending hit.
    TEST_ASSERT_EQ(ledger.book(kBoss, 300, 1000, ms(100)), 100u);
    // Long enough for any effect result to have arrived.
    TEST_ASSERT_EQ(ledger.book(kBoss, 300, 1000, ms(100) + OverkillLedger::kPendingTtlUs + 1), 0u);
}

TEST_CASE(MeterOverkill, LedgerTakesADropPastThePendingAsNone) {
    // A DoT tick's HP is never booked, so the drop can be more than what was pending.
    OverkillLedger ledger;
    TEST_ASSERT_EQ(ledger.book(kBoss, 200, 1000, ms(0)), 0u);
    TEST_ASSERT_EQ(ledger.book(kBoss, 500, 500, ms(300)), 0u);
}

TEST_CASE(MeterOverkill, LedgerKeepsEnemiesApart) {
    OverkillLedger ledger;
    TEST_ASSERT_EQ(ledger.book(kBoss, 900, 1000, ms(0)), 0u);
    TEST_ASSERT_EQ(ledger.book(kBoss + 1, 900, 1000, ms(0)), 0u);
    TEST_ASSERT_EQ(ledger.tracked_count(), 2u);
}

TEST_CASE(MeterOverkill, OnlyWhatLandedCounts) {
    // FFLogs leaves the part past the enemy's HP out of damage done.
    MetricsAccumulator accumulator;
    CombatantRegistry registry;
    hub::ipc::PartySyncPacket sync{};
    sync.entity_ids[0] = kTank;
    sync.job_ids[0] = static_cast<uint32_t>(Job::WAR);
    sync.party_count = 1;
    registry.sync_party(sync);
    accumulator.start(ms(1000));

    hub::ipc::CombatActionPacket killing_blow = hit(kTank, kBoss, 30000, 22000);
    killing_blow.credits.count = 1;
    killing_blow.credits.entries[0].giver_id = kHealer;
    killing_blow.credits.entries[0].amount = 10000;  // More than landed
    accumulator.record_action(hit(kTank, kBoss, 12000, 0), registry);
    accumulator.record_action(killing_blow, registry);
    accumulator.recalculate(10.0, &registry);

    const CombatantStats* tank = accumulator.find_stats(kTank);
    TEST_ASSERT(tank != nullptr);
    TEST_ASSERT_EQ(tank->total_damage, 20000u);
    TEST_ASSERT_EQ(accumulator.total_damage(), 20000u);
    TEST_ASSERT_EQ(accumulator.find_stats(kBoss)->damage_taken, 20000u);
    // The hit still counts, at the size the game showed it.
    TEST_ASSERT_EQ(tank->hits.total_hits, 2u);
    const ActionSummary& action = tank->actions.at(31);
    TEST_ASSERT_EQ(action.total_damage, 20000u);
    TEST_ASSERT_EQ(action.max_damage, 30000u);
    // A credit can move no more than landed.
    TEST_ASSERT_EQ(tank->buff_received, 8000u);
}

TEST_CASE(MeterOverkill, PluginSplitsAHitOnAnEnemyBeforeRecording) {
    hub::ipc::PacketRingBuffer ring;
    CombatPlugin plugin;
    plugin.initialize();
    plugin.set_ring_buffer(&ring);
    sync_party(plugin.engine());
    uint32_t boss_hp = 50000;
    plugin.set_hp_resolver([&](uint32_t entity_id, uint32_t& current_hp, uint32_t& max_hp) {
        if (entity_id == kBoss) {
            current_hp = boss_hp;
            max_hp = 100000;
            return true;
        }
        if (entity_id == kTank) {
            current_hp = 1000;
            max_hp = 100000;
            return true;
        }
        return false;
    });

    effect(plugin, kTank, kBoss, 31, 30000);
    effect(plugin, kTank, kBoss, 31, 30000);  // Only 20000 left once the first applies
    // A hit on the party is never anyone's overkill.
    effect(plugin, kBoss, kTank, 31, 5000);

    const auto sent = drain_actions(ring);
    TEST_ASSERT_EQ(sent.size(), 3u);
    TEST_ASSERT_EQ(sent[0].overkill, 0u);
    TEST_ASSERT_EQ(sent[1].overkill, 10000u);
    TEST_ASSERT_EQ(sent[2].overkill, 0u);
    TEST_ASSERT_EQ(sent[1].damage, 30000u);

    // The app's engine, fed what was sent, books the same.
    EncounterEngine app;
    sync_party(app);
    for (const auto& packet : sent) app.process_action(packet);
    const auto in_game = plugin.engine().current_summary();
    TEST_ASSERT_EQ(in_game.total_damage, 50000u);
    TEST_ASSERT_EQ(app.current_summary().total_damage, in_game.total_damage);
}

TEST_CASE(MeterOverkill, ALimitBreakFinishingTheBossBooksWhatWasLeft) {
    // A Limit Break thrown at a boss with little HP left booked its whole number.
    hub::ipc::PacketRingBuffer ring;
    CombatPlugin plugin;
    plugin.initialize();
    plugin.set_ring_buffer(&ring);
    sync_party(plugin.engine());
    plugin.set_hp_resolver([](uint32_t entity_id, uint32_t& current_hp, uint32_t& max_hp) {
        if (entity_id != kBoss) return false;
        current_hp = 9000;
        max_hp = 100000;
        return true;
    });

    effect(plugin, kTank, kBoss, kMeteor, 60000);
    const auto summary = plugin.engine().current_summary();
    const CombatantStats* lb = row(summary, hub::game::LIMIT_BREAK_COMBATANT_ID);
    TEST_ASSERT(lb != nullptr);
    TEST_ASSERT_EQ(lb->total_damage, 9000u);
}

TEST_CASE(MeterOverkill, UnreadHpBooksTheWholeHit) {
    hub::ipc::PacketRingBuffer ring;
    CombatPlugin plugin;
    plugin.initialize();
    plugin.set_ring_buffer(&ring);
    sync_party(plugin.engine());
    plugin.set_hp_resolver([](uint32_t, uint32_t&, uint32_t&) { return false; });
    effect(plugin, kTank, kBoss, 31, 30000);
    TEST_ASSERT_EQ(plugin.engine().current_summary().total_damage, 30000u);
}
