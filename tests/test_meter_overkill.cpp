#include "test_framework.hpp"
#include "meter/combat_plugin.hpp"
#include "meter/encounter_engine.hpp"
#include "meter/metrics_accumulator.hpp"
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
constexpr uint32_t kTheEnd = 24858;  // Reaper's Limit Break 3

void sync_party(EncounterEngine& engine) {
    hub::ipc::PartySyncPacket sync{};
    sync.entity_ids[0] = kTank;
    sync.job_ids[0] = static_cast<uint32_t>(Job::WAR);
    sync.entity_ids[1] = kHealer;
    sync.job_ids[1] = static_cast<uint32_t>(Job::WHM);
    sync.party_count = 2;
    engine.process_party_sync(sync);
}

/// One effect `sequence` from `source` on `target`, one entry per amount.
void effect(CombatPlugin& plugin, EntityId source, EntityId target, uint32_t action_id, uint32_t sequence,
            std::initializer_list<uint16_t> amounts) {
    hub::game::ActionEffectHeader header{};
    header.animation_target_id = target;
    header.action_id = action_id;
    header.global_sequence = sequence;
    header.num_targets = 1;
    std::array<hub::game::ActionEffectEntry, 8> entries{};
    size_t i = 0;
    for (const uint16_t amount : amounts) {
        entries[i].effect_type = 0x03;
        entries[i].value = amount;
        ++i;
    }
    plugin.on_receive_action_effect(source, nullptr, &header, entries.data(), nullptr);
}

/// Every CombatAction and CombatOverkill sent, fed in order to the app's engine.
void feed(hub::ipc::PacketRingBuffer& ring, EncounterEngine& app, std::vector<hub::ipc::OverkillPacket>* overkills = nullptr) {
    std::vector<uint8_t> frame;
    while (ring.pop(frame)) {
        const auto header = hub::ipc::deserialize_header(frame);
        if (!header) continue;
        const uint8_t* body = frame.data() + sizeof(hub::ipc::PacketHeader);
        switch (static_cast<hub::MessageType>(header->message_type)) {
            case hub::MessageType::CombatAction: {
                hub::ipc::CombatActionPacket packet{};
                std::memcpy(&packet, body, sizeof(packet));
                app.process_action(packet);
                break;
            }
            case hub::MessageType::CombatOverkill: {
                hub::ipc::OverkillPacket packet{};
                std::memcpy(&packet, body, sizeof(packet));
                app.process_overkill(packet);
                if (overkills) overkills->push_back(packet);
                break;
            }
            default: break;
        }
    }
}

const CombatantStats* row(const EncounterSummary& summary, EntityId id) {
    for (const CombatantStats& stats : summary.combatants) {
        if (stats.entity_id == id) return &stats;
    }
    return nullptr;
}

hub::ipc::CombatActionPacket hit(EntityId source, EntityId target, uint32_t damage) {
    hub::ipc::CombatActionPacket packet{};
    packet.source_id = source;
    packet.target_id = target;
    packet.action_id = 31;
    packet.damage = damage;
    packet.effect_type = static_cast<uint16_t>(EffectType::Damage);
    packet.timestamp_us = 1'000'000;
    return packet;
}

} // namespace

TEST_CASE(MeterOverkill, OnlyWhatLandedCounts) {
    // FFLogs leaves the part past the enemy's HP out of damage done.
    MetricsAccumulator accumulator;
    CombatantRegistry registry;
    hub::ipc::PartySyncPacket sync{};
    sync.entity_ids[0] = kTank;
    sync.job_ids[0] = static_cast<uint32_t>(Job::WAR);
    sync.entity_ids[1] = kHealer;
    sync.job_ids[1] = static_cast<uint32_t>(Job::WHM);
    sync.party_count = 2;
    registry.sync_party(sync);
    accumulator.start(1'000'000);

    hub::ipc::CombatActionPacket killing_blow = hit(kTank, kBoss, 30000);
    killing_blow.credits.count = 1;
    killing_blow.credits.entries[0].giver_id = kHealer;
    killing_blow.credits.entries[0].amount = 9000;
    accumulator.record_action(hit(kTank, kBoss, 12000), registry);
    accumulator.record_action(killing_blow, registry);

    hub::ipc::OverkillPacket overkill{};
    overkill.source_id = kTank;
    overkill.target_id = kBoss;
    overkill.action_id = 31;
    overkill.overkill = 22000;
    overkill.timestamp_us = killing_blow.timestamp_us;
    overkill.credits = killing_blow.credits;
    overkill.credits.entries[0].amount = 6600;  // Its share: 9000 * 22000 / 30000
    accumulator.record_overkill(overkill, registry);
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
    // The credit comes back from both sides, so the party's rDPS still adds up to its DPS.
    const CombatantStats* healer = accumulator.find_stats(kHealer);
    TEST_ASSERT_EQ(tank->buff_received, 2400u);
    TEST_ASSERT(healer != nullptr);
    TEST_ASSERT_EQ(healer->buff_given, 2400u);
    TEST_ASSERT_NEAR(tank->rdps + healer->rdps, tank->dps + healer->dps, 1e-9);
}

TEST_CASE(MeterOverkill, AHitLandsOnTheHpItsResultFinds) {
    // The End went off, then the party's hits landed during its 2.2 s animation, and
    // its result came last: FFLogs booked 449,789 of 1,482,092. Measured when it went
    // off, it kept damage those later hits had already taken.
    hub::ipc::PacketRingBuffer ring;
    CombatPlugin plugin;
    plugin.initialize();
    plugin.set_ring_buffer(&ring);
    sync_party(plugin.engine());

    effect(plugin, kTank, kBoss, kTheEnd, 100, {60000});
    effect(plugin, kTank, kBoss, 31, 101, {30000});
    plugin.on_effect_result(kBoss, 101, 50000);  // The quick hit applies first
    plugin.on_effect_result(kBoss, 100, 20000);  // The Limit Break finds 20000 left

    EncounterEngine app;
    sync_party(app);
    std::vector<hub::ipc::OverkillPacket> sent;
    feed(ring, app, &sent);
    TEST_ASSERT_EQ(sent.size(), 1u);
    TEST_ASSERT_EQ(sent[0].overkill, 40000u);
    TEST_ASSERT_EQ(sent[0].action_id, kTheEnd);

    for (const EncounterSummary& summary : {plugin.engine().current_summary(), app.current_summary()}) {
        const CombatantStats* lb = row(summary, hub::game::LIMIT_BREAK_COMBATANT_ID);
        const CombatantStats* tank = row(summary, kTank);
        TEST_ASSERT(lb != nullptr && tank != nullptr);
        TEST_ASSERT_EQ(lb->total_damage, 20000u);
        TEST_ASSERT_EQ(tank->total_damage, 30000u);
        TEST_ASSERT_EQ(summary.total_damage, 50000u);
    }
}

TEST_CASE(MeterOverkill, AnEffectsHitsLandInEntryOrder) {
    hub::ipc::PacketRingBuffer ring;
    CombatPlugin plugin;
    plugin.initialize();
    plugin.set_ring_buffer(&ring);
    sync_party(plugin.engine());

    effect(plugin, kTank, kBoss, 31, 7, {3000, 4000});
    plugin.on_effect_result(kBoss, 7, 5000);
    TEST_ASSERT_EQ(plugin.engine().current_summary().total_damage, 5000u);

    // A result comes once: the same sequence again takes nothing more.
    plugin.on_effect_result(kBoss, 7, 0);
    TEST_ASSERT_EQ(plugin.engine().current_summary().total_damage, 5000u);
}

TEST_CASE(MeterOverkill, AHitWhoseResultNeverCameStaysWhole) {
    hub::ipc::PacketRingBuffer ring;
    CombatPlugin plugin;
    plugin.initialize();
    plugin.set_ring_buffer(&ring);
    sync_party(plugin.engine());
    effect(plugin, kTank, kBoss, 31, 9, {30000});
    // Another effect's result, and another enemy's, say nothing about it.
    plugin.on_effect_result(kBoss, 10, 0);
    plugin.on_effect_result(kBoss + 1, 9, 0);
    TEST_ASSERT_EQ(plugin.engine().current_summary().total_damage, 30000u);
}

TEST_CASE(MeterOverkill, OnlyThePartysHitsOnEnemiesAreTakenBack) {
    hub::ipc::PacketRingBuffer ring;
    CombatPlugin plugin;
    plugin.initialize();
    plugin.set_ring_buffer(&ring);
    sync_party(plugin.engine());
    effect(plugin, kTank, kBoss, 31, 1, {30000});
    // The boss's hit on the tank is damage taken, and a result on the tank is no one's overkill.
    effect(plugin, kBoss, kTank, 31, 2, {5000});
    plugin.on_effect_result(kTank, 2, 0);
    std::vector<hub::ipc::OverkillPacket> sent;
    EncounterEngine app;
    sync_party(app);
    feed(ring, app, &sent);
    TEST_ASSERT(sent.empty());
    TEST_ASSERT_EQ(plugin.engine().current_summary().total_damage, 30000u);
}

TEST_CASE(MeterOverkill, ALatePullOrNoPullTakesNothing) {
    EncounterEngine engine;
    sync_party(engine);
    hub::ipc::OverkillPacket overkill{};
    overkill.source_id = kTank;
    overkill.target_id = kBoss;
    overkill.action_id = 31;
    overkill.overkill = 5000;
    overkill.timestamp_us = 1'000'000;
    engine.process_overkill(overkill);  // Out of combat
    engine.process_action(hit(kTank, kBoss, 30000));
    overkill.timestamp_us = 500'000;     // Stamped before the pull
    engine.process_overkill(overkill);
    TEST_ASSERT_EQ(engine.current_summary().total_damage, 30000u);
}
