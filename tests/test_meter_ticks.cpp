#include "test_framework.hpp"
#include "meter_test_support.hpp"
#include "hub/game/entity.hpp"
#include "hub/game/status.hpp"
#include "hub/game/tick_statuses.hpp"
#include "meter/buff_attribution.hpp"
#include "meter/combat_plugin.hpp"
#include "meter/encounter_engine.hpp"
#include "meter/tick_split.hpp"
#include <array>
#include <cstring>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace hub::meter;
using namespace hub::test::meter_support;
using hub::game::TickKind;

namespace {

constexpr EntityId kWhm = 0x10000011;
constexpr EntityId kSam = 0x10000012;
constexpr EntityId kSch = 0x10000013;
constexpr EntityId kBrd = 0x10000014;
constexpr EntityId kMonk = 0x10000015;
constexpr EntityId kBoss = 0x40000020;

// Status ids from the Status sheet (pinned below in TickStatusTableMatchesTheStatusSheet).
constexpr uint16_t kDia = 1871;            // 85 per tick
constexpr uint16_t kHiganbana = 1228;      // 50
constexpr uint16_t kCausticBite = 1200;    // 20
constexpr uint16_t kStormbite = 1201;      // 25
constexpr uint16_t kRegen = 158;           // 250
constexpr uint16_t kWhisperingDawn = 315;  // 80
constexpr uint16_t kBrotherhood = 1185;

void register_party(CombatantRegistry& registry) {
    registry.register_actor(kWhm, "Whm", Job::WHM, 0, ActorType::Player);
    registry.register_actor(kSam, "Sam", Job::SAM, 0, ActorType::Player);
    registry.register_actor(kMonk, "Monk", Job::MNK, 0, ActorType::Player);
    registry.register_actor(kBoss, "Boss", Job::None, 0, ActorType::Monster);
}

/// Every status tick sitting in a ring buffer, decoded.
std::vector<hub::ipc::StatusTickPacket> drain_ticks(hub::ipc::PacketRingBuffer& ring) {
    std::vector<hub::ipc::StatusTickPacket> out;
    std::vector<uint8_t> frame;
    while (ring.pop(frame)) {
        const auto header = hub::ipc::deserialize_header(frame);
        if (!header || header->message_type != static_cast<uint16_t>(hub::MessageType::CombatStatusTick)) continue;
        hub::ipc::StatusTickPacket tick{};
        std::memcpy(&tick, frame.data() + sizeof(hub::ipc::PacketHeader),
                    std::min(sizeof(tick), frame.size() - sizeof(hub::ipc::PacketHeader)));
        out.push_back(tick);
    }
    return out;
}

/// A hit from `source` on the boss, which opens a pull.
void hit_boss(CombatPlugin& plugin, EntityId source, uint32_t damage) {
    hub::game::ActionEffectHeader header{};
    header.animation_target_id = kBoss;
    header.action_id = 7477;
    header.num_targets = 1;
    std::array<hub::game::ActionEffectEntry, 8> entries{};
    entries[0].effect_type = 0x03;
    entries[0].value = static_cast<uint16_t>(damage);
    plugin.on_receive_action_effect(source, nullptr, &header, entries.data(), nullptr);
}

} // namespace

TEST_CASE(MeterTicks, SplitExactAddsUpToTheTotal) {
    const std::array<double, 3> thirds{1.0, 1.0, 1.0};
    auto parts = split_exact(100, thirds);
    TEST_ASSERT_EQ(parts[0], 34u);
    TEST_ASSERT_EQ(parts[1], 33u);
    TEST_ASSERT_EQ(parts[2], 33u);

    // 629.6 and 370.4: the unit rounding left over goes to the larger remainder.
    const std::array<double, 2> skewed{85.0, 50.0};
    parts = split_exact(1000, skewed);
    TEST_ASSERT_EQ(parts[0], 630u);
    TEST_ASSERT_EQ(parts[1], 370u);

    // Nothing to weigh by splits evenly, and no weights at all split nothing.
    const std::array<double, 2> none{0.0, 0.0};
    parts = split_exact(5, none);
    TEST_ASSERT_EQ(parts[0], 3u);
    TEST_ASSERT_EQ(parts[1], 2u);
    TEST_ASSERT(split_exact(7, std::span<const double>{}).empty());
}

TEST_CASE(MeterTicks, CombinedTickSplitsByPotency) {
    // The server sent one tick for both DoTs on the boss. With nobody's strength known
    // yet, potency alone decides: 85 against 50.
    TickSplitter splitter;
    const std::array statuses{status(kDia, kWhm), status(kHiganbana, kSam)};
    const auto shares = splitter.split(TickKind::Damage, 1350, 0, statuses);
    TEST_ASSERT_EQ(shares.size(), 2u);
    TEST_ASSERT_EQ(shares[0].source, kWhm);
    TEST_ASSERT_EQ(shares[0].status_id, kDia);
    TEST_ASSERT_EQ(shares[0].amount, 850u);
    TEST_ASSERT_EQ(shares[1].source, kSam);
    TEST_ASSERT_EQ(shares[1].status_id, kHiganbana);
    TEST_ASSERT_EQ(shares[1].amount, 500u);
}

TEST_CASE(MeterTicks, StrengthIsLearnedFromTicksOneSourceOwns) {
    TickSplitter splitter;

    // Only the white mage's Dia on the boss: the whole tick is theirs, 20 per point.
    auto shares = splitter.split(TickKind::Damage, 1700, 0, std::array{status(kDia, kWhm)});
    TEST_ASSERT_EQ(shares.size(), 1u);
    TEST_ASSERT_EQ(shares[0].source, kWhm);
    TEST_ASSERT_EQ(shares[0].amount, 1700u);
    TEST_ASSERT_NEAR(splitter.strength(kWhm, TickKind::Damage), 20.0, 1e-9);

    // A bard's two DoTs are one source too: 900 over 20 + 25 points, split by potency.
    shares = splitter.split(TickKind::Damage, 900, 0,
                            std::array{status(kCausticBite, kBrd), status(kStormbite, kBrd)});
    TEST_ASSERT_EQ(shares.size(), 2u);
    TEST_ASSERT_EQ(shares[0].amount, 400u);
    TEST_ASSERT_EQ(shares[1].amount, 500u);
    TEST_ASSERT_NEAR(splitter.strength(kBrd, TickKind::Damage), 20.0, 1e-9);

    // The samurai's alone, 30 per point.
    shares = splitter.split(TickKind::Damage, 1500, 0, std::array{status(kHiganbana, kSam)});
    TEST_ASSERT_NEAR(splitter.strength(kSam, TickKind::Damage), 30.0, 1e-9);

    // A source with no tick of its own yet takes the mean, and healing is learned apart.
    TEST_ASSERT_NEAR(splitter.strength(kSch, TickKind::Damage), 70.0 / 3.0, 1e-9);
    TEST_ASSERT_NEAR(splitter.strength(kSch, TickKind::Heal), 1.0, 1e-9);

    // Both on the boss: 85 x 20 against 50 x 30. A shared tick teaches nobody.
    shares = splitter.split(TickKind::Damage, 3200, 0, std::array{status(kDia, kWhm), status(kHiganbana, kSam)});
    TEST_ASSERT_EQ(shares[0].amount, 1700u);
    TEST_ASSERT_EQ(shares[1].amount, 1500u);
    TEST_ASSERT_NEAR(splitter.strength(kWhm, TickKind::Damage), 20.0, 1e-9);

    // A later tick of their own moves the estimate a step, as a crit would.
    shares = splitter.split(TickKind::Damage, 2550, 0, std::array{status(kDia, kWhm)});
    TEST_ASSERT_NEAR(splitter.strength(kWhm, TickKind::Damage), 20.0 + TickSplitter::kLearnRate * 10.0, 1e-9);
}

TEST_CASE(MeterTicks, HotOverhealIsSplitWithTheShares) {
    TickSplitter splitter;
    const std::array statuses{status(kRegen, kWhm), status(kWhisperingDawn, kSch)};
    const auto shares = splitter.split(TickKind::Heal, 3300, 330, statuses);
    TEST_ASSERT_EQ(shares.size(), 2u);
    TEST_ASSERT_EQ(shares[0].source, kWhm);
    TEST_ASSERT_EQ(shares[0].amount, 2500u);
    TEST_ASSERT_EQ(shares[0].overheal, 250u);
    TEST_ASSERT_EQ(shares[1].source, kSch);
    TEST_ASSERT_EQ(shares[1].amount, 800u);
    TEST_ASSERT_EQ(shares[1].overheal, 80u);
}

TEST_CASE(MeterTicks, NothingToSplitAcrossKeepsTheTickWhole) {
    TickSplitter splitter;
    // A raid buff ticks for nobody, a HoT has no part in a damage tick, and a status
    // with no real source cannot be anyone's.
    const std::array statuses{status(kBrotherhood, kSam), status(kRegen, kWhm),
                              status(kDia, hub::game::NO_ENTITY_ID)};
    TEST_ASSERT(splitter.split(TickKind::Damage, 1000, 0, statuses).empty());
    TEST_ASSERT(splitter.split(TickKind::Damage, 0, 0, std::array{status(kDia, kWhm)}).empty());
}

TEST_CASE(MeterTicks, AStatusSeenTickingAloneTakesNoShare) {
    // A status whose ticks name it, as a ground effect's do, is not in the combined tick.
    TickSplitter splitter;
    splitter.note_own_tick(kDia);
    const auto shares = splitter.split(TickKind::Damage, 1000, 0,
                                       std::array{status(kDia, kWhm), status(kHiganbana, kSam)});
    TEST_ASSERT_EQ(shares.size(), 1u);
    TEST_ASSERT_EQ(shares[0].source, kSam);
    TEST_ASSERT_EQ(shares[0].amount, 1000u);
}

TEST_CASE(MeterTicks, PluginSplitsACombinedTickAndShipsEachShare) {
    CombatPlugin plugin;
    plugin.initialize();
    hub::ipc::PacketRingBuffer ring;
    plugin.set_ring_buffer(&ring);
    register_party(plugin.engine().registry_unlocked());
    FakeStatuses statuses;
    statuses.by_actor[kBoss] = {status(kDia, kWhm), status(kHiganbana, kSam)};
    plugin.set_status_reader(statuses.reader());

    hit_boss(plugin, kSam, 1000);
    drain_ticks(ring);

    // The server's tick: no status, both DoTs summed, named after the white mage.
    // Booked whole, the samurai's DoT damage was the white mage's.
    plugin.on_status_tick(kBoss, kWhm, 0, 1350, /*is_heal=*/false);
    const auto ticks = drain_ticks(ring);
    TEST_ASSERT_EQ(ticks.size(), 2u);
    TEST_ASSERT_EQ(ticks[0].source_id, kWhm);
    TEST_ASSERT_EQ(ticks[0].status_id, kDia);
    TEST_ASSERT_EQ(ticks[0].damage_or_heal, 850u);
    TEST_ASSERT_EQ(ticks[1].source_id, kSam);
    TEST_ASSERT_EQ(ticks[1].status_id, kHiganbana);
    TEST_ASSERT_EQ(ticks[1].damage_or_heal, 500u);

    const MetricsAccumulator& acc = plugin.engine().accumulator_unlocked();
    TEST_ASSERT_EQ(acc.find_stats(kWhm)->total_damage, 850u);
    TEST_ASSERT_EQ(acc.find_stats(kSam)->total_damage, 1500u);
    TEST_ASSERT_EQ(acc.total_damage(), 2350u);

    // The app's engine books the shares it was shipped to the same numbers.
    EncounterEngine mirror;
    register_party(mirror.registry_unlocked());
    hub::ipc::CombatActionPacket opener{};
    opener.source_id = kSam;
    opener.target_id = kBoss;
    opener.damage = 1000;
    opener.effect_type = static_cast<uint16_t>(EffectType::Damage);
    mirror.process_action(opener);
    for (const auto& tick : ticks) mirror.process_status_tick(tick);
    TEST_ASSERT_EQ(mirror.accumulator_unlocked().find_stats(kWhm)->total_damage, 850u);
    TEST_ASSERT_EQ(mirror.accumulator_unlocked().find_stats(kSam)->total_damage, 1500u);

    // Nothing on the boss to split across: the tick stays whole, with its named source.
    statuses.by_actor[kBoss].clear();
    plugin.on_status_tick(kBoss, kWhm, 0, 700, false);
    auto whole = drain_ticks(ring);
    TEST_ASSERT_EQ(whole.size(), 1u);
    TEST_ASSERT_EQ(whole[0].source_id, kWhm);
    TEST_ASSERT_EQ(whole[0].status_id, 0u);
    TEST_ASSERT_EQ(whole[0].damage_or_heal, 700u);

    // A tick that names its status, a ground effect's, is never split.
    statuses.by_actor[kBoss] = {status(kDia, kWhm), status(kHiganbana, kSam)};
    plugin.on_status_tick(kBoss, kSam, 501, 300, false);
    whole = drain_ticks(ring);
    TEST_ASSERT_EQ(whole.size(), 1u);
    TEST_ASSERT_EQ(whole[0].source_id, kSam);
    TEST_ASSERT_EQ(whole[0].status_id, 501u);
}

TEST_CASE(MeterTicks, SplitSharesEarnTheirDotsCredits) {
    // A combined tick never matched a DoT's snapshot, keyed by its status, so DoT
    // damage earned no buff credits at all.
    CombatPlugin plugin;
    plugin.initialize();
    register_party(plugin.engine().registry_unlocked());
    FakeStatuses statuses;
    statuses.by_actor[kSam] = {status(kBrotherhood, kMonk)};
    plugin.set_status_reader(statuses.reader());

    // Higanbana applied under Brotherhood (effect kind 14 on the boss).
    hub::game::ActionEffectHeader header{};
    header.animation_target_id = kBoss;
    header.action_id = 7489;
    header.num_targets = 1;
    std::array<hub::game::ActionEffectEntry, 8> entries{};
    entries[0].effect_type = 0x03;
    entries[0].value = 2000;
    entries[1].effect_type = 0x0E;
    entries[1].value = kHiganbana;
    plugin.on_receive_action_effect(kSam, nullptr, &header, entries.data(), nullptr);
    const double hit_credit = 2000.0 - 2000.0 / 1.05;

    // Brotherhood wears off; the combined tick's share still carries it.
    statuses.by_actor[kSam].clear();
    statuses.by_actor[kBoss] = {status(kHiganbana, kSam)};
    plugin.on_status_tick(kBoss, kSam, 0, 4200, /*is_heal=*/false);
    TEST_ASSERT_NEAR(static_cast<double>(plugin.engine().accumulator_unlocked().find_stats(kMonk)->buff_given),
                     hit_credit + (4200.0 - 4200.0 / 1.05), 2.0);
}

TEST_CASE(MeterGameData, TickStatusTableMatchesTheStatusSheet) {
    // Ids drift between patches; the names pin each one to what the table means.
    const std::unordered_map<uint32_t, std::string_view> expected{
        {118, "Chaos Thrust"}, {124, "Venomous Bite"}, {129, "Windbite"}, {143, "Aero"}, {144, "Aero II"},
        {150, "Medica II"}, {158, "Regen"}, {161, "Thunder"}, {162, "Thunder II"}, {163, "Thunder III"},
        {179, "Bio"}, {189, "Bio II"}, {248, "Circle of Scorn"}, {315, "Whispering Dawn"},
        {835, "Aspected Benefic"}, {836, "Aspected Helios"}, {838, "Combust"}, {843, "Combust II"},
        {956, "Wheel of Fortune"}, {1200, "Caustic Bite"}, {1201, "Stormbite"}, {1210, "Thunder IV"},
        {1228, "Higanbana"}, {1835, "Aurora"}, {1837, "Sonic Break"}, {1838, "Bow Shock"}, {1866, "Bioblaster"},
        {1871, "Dia"}, {1874, "Angel's Whisper"}, {1879, "Opposition"}, {1881, "Combust III"},
        {1895, "Biolysis"}, {2108, "Shake It Off (Over Time)"}, {2614, "Eukrasian Dosis"},
        {2615, "Eukrasian Dosis II"}, {2616, "Eukrasian Dosis III"}, {2617, "Physis"}, {2620, "Physis II"},
        {2676, "Knight's Benediction"}, {2695, "Improvisation"}, {2705, "Undying Flame"},
        {2719, "Chaotic Spring"}, {2938, "Kerakeia"}, {3871, "High Thunder"}, {3872, "High Thunder"},
        {3880, "Medica III"}, {3883, "Baneful Impaction"}, {3885, "Seraphism"}, {3891, "The Ewer"},
        {3894, "Helios Conjunction"}, {3900, "Primeval Impulse"}, {3904, "Divine Aura"},
    };
    TEST_ASSERT_EQ(hub::game::TICK_STATUSES.size(), expected.size());
    for (size_t i = 0; i < hub::game::TICK_STATUSES.size(); ++i) {
        const auto& tick = hub::game::TICK_STATUSES[i];
        if (i > 0) TEST_ASSERT(hub::game::TICK_STATUSES[i - 1].status_id < tick.status_id);
        TEST_ASSERT_EQ(hub::game::status_sheet_name(tick.status_id), expected.at(tick.status_id));
        TEST_ASSERT(hub::game::find_tick_status(tick.status_id) == &tick);
        TEST_ASSERT(tick.potency > 0);
        // The payload's status read has to keep it for the split to see it.
        TEST_ASSERT(is_attribution_status(tick.status_id));
    }
    TEST_ASSERT(hub::game::find_tick_status(kBrotherhood) == nullptr);
    TEST_ASSERT(hub::game::find_tick_status(1911) == nullptr); // Asylum, a ground effect
}
