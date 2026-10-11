#include "test_framework.hpp"
#include "meter_test_support.hpp"
#include "hub/game/guaranteed_hits.hpp"
#include "hub/game/raid_buffs.hpp"
#include "hub/game/status.hpp"
#include "meter/action_decoder.hpp"
#include "meter/buff_attribution.hpp"
#include "meter/combat_plugin.hpp"
#include "meter/encounter_engine.hpp"
#include "common/config/json.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

using namespace hub::meter;
using namespace hub::test::meter_support;

namespace {

constexpr EntityId kSam = 0x10000001;
constexpr EntityId kMonk = 0x10000002;
constexpr EntityId kSage = 0x10000003;
constexpr EntityId kBoss = 0x40000010;

// Status ids from the Status sheet (pinned below in RaidBuffTableMatchesTheStatusSheet).
constexpr uint16_t kBattleVoice = 141;
constexpr uint16_t kBattleLitany = 786;
constexpr uint16_t kBrotherhood = 1185;
constexpr uint16_t kChainStratagem = 1221;
constexpr uint16_t kTechnicalFinish = 1822;
constexpr uint16_t kDevilment = 1825;
constexpr uint16_t kDivination = 1878;
constexpr uint16_t kRadiantFinale = 2964;
constexpr uint16_t kDokumori = 3849;
constexpr uint16_t kTheBalance = 3887;
constexpr uint16_t kTheSpear = 3889;
constexpr uint16_t kLifeSurge = 116;
constexpr uint16_t kOpoOpoForm = 107;

// With no clean hits seen, the estimate is the prior: c = 1.35 + 0.22.
constexpr double kPriorCritMultiplier = 1.35 + RateEstimator::kPriorCrit;

AttributedHit hit(uint32_t damage, bool crit = false, bool direct_hit = false, ActionId action = 7477,
                  Role role = Role::Melee) {
    AttributedHit h;
    h.source = kSam;
    h.source_role = role;
    h.action_id = action;
    h.damage = damage;
    h.crit = crit;
    h.direct_hit = direct_hit;
    return h;
}

const hub::ipc::CombatBuffCredit* credit_of(const Attribution& a, EntityId giver, bool single = false) {
    for (uint8_t i = 0; i < a.credits.count; ++i) {
        const auto& c = a.credits.entries[i];
        if (c.giver_id == giver && (c.single_target != 0) == single) return &c;
    }
    return nullptr;
}

/// The credit a buff multiplying damage by `m` earns on a hit of `damage`.
double expected_credit(double damage, double m) {
    return damage - damage / m;
}

void register_party(CombatantRegistry& registry) {
    registry.register_actor(kSam, "Sam", Job::SAM, 0, ActorType::Player);
    registry.register_actor(kMonk, "Monk", Job::MNK, 0, ActorType::Player);
    registry.register_actor(kSage, "Sage", Job::SGE, 0, ActorType::Player);
    registry.register_actor(kBoss, "Boss", Job::None, 0, ActorType::Monster);
}

std::vector<hub::ipc::CombatActionPacket> drain_actions(hub::ipc::PacketRingBuffer& ring) {
    std::vector<hub::ipc::CombatActionPacket> out;
    std::vector<uint8_t> frame;
    while (ring.pop(frame)) {
        const auto header = hub::ipc::deserialize_header(frame);
        if (!header || header->message_type != static_cast<uint16_t>(hub::MessageType::CombatAction)) continue;
        hub::ipc::CombatActionPacket packet{};
        std::memcpy(&packet, frame.data() + sizeof(hub::ipc::PacketHeader),
                    std::min(sizeof(packet), frame.size() - sizeof(hub::ipc::PacketHeader)));
        out.push_back(packet);
    }
    return out;
}

const CombatantStats* row_of(const EncounterSummary& summary, EntityId id) {
    for (const auto& c : summary.combatants) {
        if (c.entity_id == id) return &c;
    }
    return nullptr;
}

} // namespace

TEST_CASE(MeterRdps, DamageBuffCreditsItsGiver) {
    RateEstimator rates;
    BuffStrengths strengths;
    const std::array statuses{status(kBrotherhood, kMonk)};

    const Attribution a = attribute_hit(hit(10500), statuses, {}, rates, strengths);
    TEST_ASSERT_EQ(a.credits.count, 1u);
    const auto* monk = credit_of(a, kMonk);
    TEST_ASSERT(monk != nullptr);
    TEST_ASSERT_NEAR(static_cast<double>(monk->amount), 500.0, 1.0);
    TEST_ASSERT_EQ(credited_total(a.credits), static_cast<uint64_t>(monk->amount));
    // A damage buff does not touch the rates, so the hit still shows the attacker's own.
    TEST_ASSERT(a.clean);
}

TEST_CASE(MeterRdps, StackedBuffsSplitByLogShare) {
    RateEstimator rates;
    BuffStrengths strengths;
    const std::array statuses{status(kBrotherhood, kMonk), status(kDivination, kSage)};

    const double damage = 11130.0; // 10000 x 1.05 x 1.06
    const Attribution a = attribute_hit(hit(11130), statuses, {}, rates, strengths);
    const double bonus = expected_credit(damage, 1.05 * 1.06);
    const double log_total = std::log(1.05 * 1.06);
    TEST_ASSERT_NEAR(static_cast<double>(credit_of(a, kMonk)->amount), bonus * std::log(1.05) / log_total, 1.0);
    TEST_ASSERT_NEAR(static_cast<double>(credit_of(a, kSage)->amount), bonus * std::log(1.06) / log_total, 1.0);
    // Credits never exceed what the buffs added.
    TEST_ASSERT(credited_total(a.credits) <= static_cast<uint64_t>(bonus));
}

TEST_CASE(MeterRdps, CritBuffEarnsOnlyWhenTheHitCrits) {
    RateEstimator rates;
    BuffStrengths strengths;
    const std::array statuses{status(kBattleLitany, kMonk)};

    const Attribution plain = attribute_hit(hit(10000), statuses, {}, rates, strengths);
    TEST_ASSERT_EQ(plain.credits.count, 0u);
    TEST_ASSERT_FALSE(plain.clean);

    // The crit's bonus is shared by everything that made the crit likely.
    const Attribution crit = attribute_hit(hit(10000, /*crit=*/true), statuses, {}, rates, strengths);
    const double share = 0.10 / (RateEstimator::kPriorCrit + 0.10);
    TEST_ASSERT_NEAR(static_cast<double>(credit_of(crit, kMonk)->amount),
                     expected_credit(10000.0, std::pow(kPriorCritMultiplier, share)), 1.0);
}

TEST_CASE(MeterRdps, DirectHitBuffUsesTheFixedBonus) {
    RateEstimator rates;
    BuffStrengths strengths;
    const std::array statuses{status(kBattleVoice, kMonk)};

    const Attribution a = attribute_hit(hit(12500, false, /*direct_hit=*/true), statuses, {}, rates, strengths);
    const double share = 0.20 / (RateEstimator::kPriorDirectHit + 0.20);
    TEST_ASSERT_NEAR(static_cast<double>(credit_of(a, kMonk)->amount),
                     expected_credit(12500.0, std::pow(1.25, share)), 1.0);
}

TEST_CASE(MeterRdps, GuaranteedHitsTurnRatesIntoDamage) {
    RateEstimator rates;
    BuffStrengths strengths;
    const std::array litany{status(kBattleLitany, kMonk)};
    const double converted = 1.0 + 0.10 * (kPriorCritMultiplier - 1.0);

    // Midare Setsugekka always crits.
    const Attribution midare = attribute_hit(hit(10000, true, false, 7487), litany, {}, rates, strengths);
    TEST_ASSERT_NEAR(static_cast<double>(credit_of(midare, kMonk)->amount), expected_credit(10000.0, converted), 1.0);
    TEST_ASSERT_FALSE(midare.clean);

    // Life Surge guarantees the next weaponskill, so only a GCD.
    const std::array surged{status(kBattleLitany, kMonk), status(kLifeSurge, kSam)};
    const Attribution weaponskill = attribute_hit(hit(10000, true, false, 31), surged, {}, rates, strengths);
    TEST_ASSERT_NEAR(static_cast<double>(credit_of(weaponskill, kMonk)->amount),
                     expected_credit(10000.0, converted), 1.0);
    const Attribution ability = attribute_hit(hit(10000, true, false, 38), surged, {}, rates, strengths);
    TEST_ASSERT(credit_of(ability, kMonk)->amount != credit_of(weaponskill, kMonk)->amount);

    // Leaping Opo only with its form bonus.
    const std::array in_form{status(kBattleLitany, kMonk), status(kOpoOpoForm, kSam)};
    const Attribution opo = attribute_hit(hit(10000, true, false, 36945), in_form, {}, rates, strengths);
    TEST_ASSERT_NEAR(static_cast<double>(credit_of(opo, kMonk)->amount), expected_credit(10000.0, converted), 1.0);
}

TEST_CASE(MeterRdps, OwnBuffsEarnNothing) {
    RateEstimator rates;
    BuffStrengths strengths;
    const std::array own_damage{status(kTechnicalFinish, kSam)};
    const Attribution a = attribute_hit(hit(10000), own_damage, {}, rates, strengths);
    TEST_ASSERT_EQ(a.credits.count, 0u);
    TEST_ASSERT(a.clean);

    // Its own rate buff still made the crit likelier, so the hit is not clean.
    const std::array own_rate{status(kDevilment, kSam)};
    const Attribution b = attribute_hit(hit(10000, true), own_rate, {}, rates, strengths);
    TEST_ASSERT_EQ(b.credits.count, 0u);
    TEST_ASSERT_FALSE(b.clean);
}

TEST_CASE(MeterRdps, OwnRateBuffShrinksAnotherPlayersShare) {
    RateEstimator rates;
    BuffStrengths strengths;
    const std::array statuses{status(kBattleLitany, kMonk), status(kDevilment, kSam)};
    const Attribution a = attribute_hit(hit(10000, true), statuses, {}, rates, strengths);
    const double share = 0.10 / (RateEstimator::kPriorCrit + 0.10 + 0.20);
    TEST_ASSERT_NEAR(static_cast<double>(credit_of(a, kMonk)->amount),
                     expected_credit(10000.0, std::pow(kPriorCritMultiplier, share)), 1.0);
}

TEST_CASE(MeterRdps, CardStrengthFollowsTheReceiversRole) {
    RateEstimator rates;
    BuffStrengths strengths;
    const std::array balance{status(kTheBalance, kSage)};
    const std::array spear{status(kTheSpear, kSage)};

    const Attribution melee = attribute_hit(hit(10600, false, false, 7477, Role::Melee), balance, {}, rates, strengths);
    TEST_ASSERT_NEAR(static_cast<double>(credit_of(melee, kSage, true)->amount), 600.0, 1.0);
    const Attribution caster = attribute_hit(hit(10300, false, false, 7477, Role::Caster), balance, {}, rates, strengths);
    TEST_ASSERT_NEAR(static_cast<double>(credit_of(caster, kSage, true)->amount), 300.0, 1.0);

    const Attribution ranged = attribute_hit(hit(10600, false, false, 7477, Role::Ranged), spear, {}, rates, strengths);
    TEST_ASSERT_NEAR(static_cast<double>(credit_of(ranged, kSage, true)->amount), 600.0, 1.0);
    const Attribution tank = attribute_hit(hit(10300, false, false, 7477, Role::Tank), spear, {}, rates, strengths);
    TEST_ASSERT_NEAR(static_cast<double>(credit_of(tank, kSage, true)->amount), 300.0, 1.0);
}

TEST_CASE(MeterRdps, FinishAndFinaleStrengthComeFromTheirAction) {
    RateEstimator rates;
    BuffStrengths strengths;
    const std::array technical{status(kTechnicalFinish, kMonk)};

    // Never seen danced: the full four-step strength.
    TEST_ASSERT_NEAR(static_cast<double>(credit_of(attribute_hit(hit(10500), technical, {}, rates, strengths), kMonk)->amount),
                     500.0, 1.0);
    strengths.on_action(kMonk, 16194); // Double Technical Finish
    TEST_ASSERT_NEAR(static_cast<double>(credit_of(attribute_hit(hit(10200), technical, {}, rates, strengths), kMonk)->amount),
                     200.0, 1.0);

    // Two different songs sung since the last finale: two codas.
    const std::array finale{status(kRadiantFinale, kSage)};
    strengths.on_action(kSage, 114);
    strengths.on_action(kSage, 3559);
    strengths.on_action(kSage, 114);
    strengths.on_action(kSage, hub::game::RADIANT_FINALE_ACTION);
    TEST_ASSERT_NEAR(static_cast<double>(credit_of(attribute_hit(hit(10400), finale, {}, rates, strengths), kSage)->amount),
                     400.0, 1.0);
    // A finale whose songs were missed falls back to the full strength.
    strengths.on_action(kSage, hub::game::RADIANT_FINALE_ACTION);
    TEST_ASSERT_NEAR(static_cast<double>(credit_of(attribute_hit(hit(10600), finale, {}, rates, strengths), kSage)->amount),
                     600.0, 1.0);
}

TEST_CASE(MeterRdps, EnemyDebuffsCountOnlyOnTheTarget) {
    RateEstimator rates;
    BuffStrengths strengths;
    const std::array on_boss{status(kDokumori, kMonk), status(kChainStratagem, kSage)};

    const Attribution a = attribute_hit(hit(10000, true), {}, on_boss, rates, strengths);
    TEST_ASSERT(credit_of(a, kMonk) != nullptr);
    TEST_ASSERT(credit_of(a, kSage) != nullptr);

    // The same statuses on the attacker are not what they mean.
    const Attribution misplaced = attribute_hit(hit(10000, true), on_boss, {}, rates, strengths);
    TEST_ASSERT_EQ(misplaced.credits.count, 0u);
}

TEST_CASE(MeterRdps, TicksEarnTheExpectedValue) {
    RateEstimator rates;
    BuffStrengths strengths;
    const std::array statuses{status(kBattleLitany, kMonk)};
    AttributedHit tick = hit(10000);
    tick.is_tick = true;

    const Attribution a = attribute_hit(tick, statuses, {}, rates, strengths);
    const auto expected = [](double rate) { return 1.0 + rate * (kPriorCritMultiplier - 1.0); };
    const double m = expected(RateEstimator::kPriorCrit + 0.10) / expected(RateEstimator::kPriorCrit);
    TEST_ASSERT_NEAR(static_cast<double>(credit_of(a, kMonk)->amount), expected_credit(10000.0, m), 1.0);
    TEST_ASSERT_FALSE(a.clean);
}

TEST_CASE(MeterRdps, RateEstimateStartsAtThePriorAndStaysInRange) {
    RateEstimator rates;
    TEST_ASSERT_NEAR(rates.crit_rate(kSam), RateEstimator::kPriorCrit, 1e-9);
    TEST_ASSERT_NEAR(rates.direct_hit_rate(kSam), RateEstimator::kPriorDirectHit, 1e-9);

    for (int i = 0; i < 1000; ++i) rates.observe(kSam, i % 2 == 0, false);
    TEST_ASSERT_NEAR(rates.crit_rate(kSam), (500.0 + RateEstimator::kPriorCrit * 100.0) / 1100.0, 1e-9);
    TEST_ASSERT_NEAR(rates.direct_hit_rate(kSam), RateEstimator::kMinRate, 1e-9);

    for (int i = 0; i < 5000; ++i) rates.observe(kMonk, true, true);
    TEST_ASSERT_NEAR(rates.crit_rate(kMonk), RateEstimator::kMaxRate, 1e-9);
}

TEST_CASE(MeterRdps, CreditsMoveDamageWithoutCreatingIt) {
    EncounterEngine engine;
    register_party(engine.registry_unlocked());
    const auto start = std::chrono::steady_clock::now();

    hub::ipc::CombatActionPacket sam_hit{};
    sam_hit.source_id = kSam;
    sam_hit.target_id = kBoss;
    sam_hit.action_id = 7477;
    sam_hit.damage = 10000;
    sam_hit.effect_type = static_cast<uint16_t>(EffectType::Damage);
    sam_hit.credits.count = 2;
    sam_hit.credits.entries[0] = {kMonk, 500, 0, {}};
    sam_hit.credits.entries[1] = {kSage, 300, 1, {}};
    engine.process_action(sam_hit, start);

    hub::ipc::CombatActionPacket monk_hit = sam_hit;
    monk_hit.source_id = kMonk;
    monk_hit.damage = 4000;
    monk_hit.credits = {};
    engine.process_action(monk_hit, start);

    engine.end_encounter(EncounterEndReason::Manual, start + std::chrono::seconds(10));
    const auto pull = engine.latest_pull();
    TEST_ASSERT(pull.has_value());

    const CombatantStats* sam = row_of(*pull, kSam);
    const CombatantStats* monk = row_of(*pull, kMonk);
    const CombatantStats* sage = row_of(*pull, kSage);
    TEST_ASSERT(sam != nullptr && monk != nullptr);
    // The sage dealt nothing but its buffs did, so it still ranks.
    TEST_ASSERT(sage != nullptr);

    TEST_ASSERT_NEAR(sam->dps, 1000.0, 1e-6);
    TEST_ASSERT_NEAR(sam->rdps, 920.0, 1e-6);
    TEST_ASSERT_NEAR(sam->adps, 970.0, 1e-6);
    TEST_ASSERT_NEAR(sam->ndps, 920.0, 1e-6);
    TEST_ASSERT_NEAR(sam->cdps, 970.0, 1e-6);
    TEST_ASSERT_NEAR(monk->rdps, 450.0, 1e-6);
    TEST_ASSERT_NEAR(monk->cdps, 450.0, 1e-6);
    TEST_ASSERT_NEAR(monk->ndps, 400.0, 1e-6);
    TEST_ASSERT_NEAR(sage->rdps, 30.0, 1e-6);

    double dps = 0.0, rdps = 0.0;
    for (const auto& c : pull->combatants) {
        dps += c.dps;
        rdps += c.rdps;
    }
    TEST_ASSERT_NEAR(rdps, dps, 1e-6);

    TEST_ASSERT_EQ(pull->buff_credits.size(), 2u);
    TEST_ASSERT_EQ(pull->buff_credits[0].giver, kMonk);
    TEST_ASSERT_EQ(pull->buff_credits[0].amount, 500u);
    TEST_ASSERT(pull->buff_credits[1].single_target);
}

TEST_CASE(MeterRdps, LimitBreakAndOversizedCreditsAreIgnored) {
    EncounterEngine engine;
    register_party(engine.registry_unlocked());
    const auto start = std::chrono::steady_clock::now();

    hub::ipc::CombatActionPacket limit_break{};
    limit_break.source_id = kSam;
    limit_break.target_id = kBoss;
    limit_break.action_id = hub::game::LIMIT_BREAK_ACTIONS.front();
    limit_break.damage = 90000;
    limit_break.effect_type = static_cast<uint16_t>(EffectType::Damage);
    limit_break.credits.count = 1;
    limit_break.credits.entries[0] = {kMonk, 5000, 0, {}};
    engine.process_action(limit_break, start);

    // A credit can never be more than the hit it came from.
    hub::ipc::CombatActionPacket greedy = limit_break;
    greedy.action_id = 7477;
    greedy.damage = 1000;
    greedy.credits.count = 2;
    greedy.credits.entries[0] = {kMonk, 800, 0, {}};
    greedy.credits.entries[1] = {kSage, 800, 0, {}};
    engine.process_action(greedy, start);

    engine.end_encounter(EncounterEndReason::Manual, start + std::chrono::seconds(10));
    const auto pull = engine.latest_pull();
    const CombatantStats* sam = row_of(*pull, kSam);
    const CombatantStats* monk = row_of(*pull, kMonk);
    TEST_ASSERT_EQ(sam->buff_received, 1000u);
    TEST_ASSERT_EQ(monk->buff_given, 800u);
    for (const auto& row : pull->buff_credits) {
        TEST_ASSERT(row.receiver != hub::game::LIMIT_BREAK_COMBATANT_ID);
    }
}

TEST_CASE(MeterRdps, LatePetMergeMovesItsCredits) {
    EncounterEngine engine;
    register_party(engine.registry_unlocked());
    constexpr EntityId kPet = 0x40000050;
    engine.registry_unlocked().register_actor(kPet, "Automaton Queen", Job::None, 0, ActorType::Pet);
    const auto start = std::chrono::steady_clock::now();

    hub::ipc::CombatActionPacket opener{};
    opener.source_id = kSam;
    opener.target_id = kBoss;
    opener.action_id = 7477;
    opener.damage = 1000;
    opener.effect_type = static_cast<uint16_t>(EffectType::Damage);
    engine.process_action(opener, start);

    // Booked on the pet's own row before anyone knew whose pet it was.
    hub::ipc::CombatActionPacket pet_hit = opener;
    pet_hit.source_id = kPet;
    pet_hit.damage = 2100;
    pet_hit.credits.count = 1;
    pet_hit.credits.entries[0] = {kMonk, 100, 0, {}};
    engine.process_action(pet_hit, start);

    engine.registry_unlocked().set_pet_owner(kPet, kSam);
    engine.end_encounter(EncounterEndReason::Manual, start + std::chrono::seconds(10));
    const auto pull = engine.latest_pull();
    const CombatantStats* sam = row_of(*pull, kSam);
    TEST_ASSERT_EQ(sam->buff_received, 100u);
    TEST_ASSERT_EQ(pull->buff_credits.size(), 1u);
    TEST_ASSERT_EQ(pull->buff_credits[0].receiver, kSam);
}

TEST_CASE(MeterRdps, PluginCreditsHitsAndShipsThem) {
    CombatPlugin plugin;
    plugin.initialize();
    hub::ipc::PacketRingBuffer ring;
    plugin.set_ring_buffer(&ring);
    register_party(plugin.engine().registry_unlocked());

    FakeStatuses statuses;
    statuses.by_actor[kSam] = {status(kBrotherhood, kMonk)};
    plugin.set_status_reader(statuses.reader());

    hub::game::ActionEffectHeader header{};
    header.animation_target_id = kBoss;
    header.action_id = 7477;
    header.num_targets = 1;
    std::array<hub::game::ActionEffectEntry, 8> entries{};
    entries[0].effect_type = 0x03;
    entries[0].value = 10500;

    plugin.on_receive_action_effect(kSam, nullptr, &header, entries.data(), nullptr);

    const auto shipped = drain_actions(ring);
    TEST_ASSERT_EQ(shipped.size(), 1u);
    TEST_ASSERT_EQ(shipped[0].credits.count, 1u);
    TEST_ASSERT_EQ(shipped[0].credits.entries[0].giver_id, kMonk);
    TEST_ASSERT_NEAR(static_cast<double>(shipped[0].credits.entries[0].amount), 500.0, 1.0);

    // The app's engine books what was shipped and lands on the same numbers.
    EncounterEngine mirror;
    register_party(mirror.registry_unlocked());
    mirror.process_action(shipped[0]);
    const auto* sam_in_game = plugin.engine().accumulator_unlocked().find_stats(kSam);
    const auto* sam_mirrored = mirror.accumulator_unlocked().find_stats(kSam);
    TEST_ASSERT_EQ(sam_in_game->buff_received, sam_mirrored->buff_received);
    TEST_ASSERT_EQ(plugin.engine().accumulator_unlocked().find_stats(kMonk)->buff_given,
                   mirror.accumulator_unlocked().find_stats(kMonk)->buff_given);
}

TEST_CASE(MeterRdps, UnreadableStatusesEarnNothing) {
    CombatPlugin plugin;
    plugin.initialize();
    register_party(plugin.engine().registry_unlocked());
    plugin.set_status_reader([](uint32_t, const void*, std::span<hub::ipc::CombatStatusEntry>) {
        return std::optional<size_t>{};
    });

    hub::game::ActionEffectHeader header{};
    header.animation_target_id = kBoss;
    header.action_id = 7477;
    header.num_targets = 1;
    std::array<hub::game::ActionEffectEntry, 8> entries{};
    entries[0].effect_type = 0x03;
    entries[0].value = 10500;
    plugin.on_receive_action_effect(kSam, nullptr, &header, entries.data(), nullptr);

    TEST_ASSERT_EQ(plugin.engine().accumulator_unlocked().find_stats(kSam)->buff_received, 0u);
}

TEST_CASE(MeterRdps, ReflectsEarnNoCredits) {
    // The statuses read are the caster's, and a reflect is the target's hit: crediting
    // it would hand the boss's buffs, or none, to the wrong hit.
    CombatPlugin plugin;
    plugin.initialize();
    hub::ipc::PacketRingBuffer ring;
    plugin.set_ring_buffer(&ring);
    register_party(plugin.engine().registry_unlocked());
    FakeStatuses statuses;
    statuses.by_actor[kSam] = {status(kBrotherhood, kMonk)};
    plugin.set_status_reader(statuses.reader());

    hub::game::ActionEffectHeader header{};
    header.animation_target_id = kSam;
    header.action_id = 7;
    header.num_targets = 1;
    std::array<hub::game::ActionEffectEntry, 8> entries{};
    entries[0].effect_type = 0x03;
    entries[0].value = 3000;
    entries[1].effect_type = 0x03;
    entries[1].value = 800;
    entries[1].flags = 0xA0;
    plugin.on_receive_action_effect(kBoss, nullptr, &header, entries.data(), nullptr);

    const auto shipped = drain_actions(ring);
    TEST_ASSERT_EQ(shipped.size(), 2u);
    TEST_ASSERT_EQ(shipped[1].source_id, kSam);
    TEST_ASSERT_EQ(shipped[1].credits.count, 0u);
    TEST_ASSERT_EQ(plugin.engine().accumulator_unlocked().find_stats(kSam)->total_damage, 800u);
    TEST_ASSERT_EQ(plugin.engine().accumulator_unlocked().find_stats(kSam)->buff_received, 0u);
}

TEST_CASE(MeterRdps, DotTicksKeepTheBuffsTheyWereAppliedUnder) {
    CombatPlugin plugin;
    plugin.initialize();
    register_party(plugin.engine().registry_unlocked());
    FakeStatuses statuses;
    statuses.by_actor[kSam] = {status(kBrotherhood, kMonk)};
    plugin.set_status_reader(statuses.reader());

    // An attack that also applies a DoT (effect kind 14) to the boss.
    constexpr uint16_t kDot = 1228; // Higanbana
    hub::game::ActionEffectHeader header{};
    header.animation_target_id = kBoss;
    header.action_id = 7489;
    header.num_targets = 1;
    std::array<hub::game::ActionEffectEntry, 8> entries{};
    entries[0].effect_type = 0x03;
    entries[0].value = 2000;
    entries[1].effect_type = 0x0E;
    entries[1].value = kDot;
    plugin.on_receive_action_effect(kSam, nullptr, &header, entries.data(), nullptr);

    // Brotherhood wears off; the ticks still carry it.
    statuses.by_actor[kSam].clear();
    plugin.on_status_tick(kBoss, kSam, kDot, 4200, /*is_heal=*/false);
    TEST_ASSERT_NEAR(static_cast<double>(plugin.engine().accumulator_unlocked().find_stats(kMonk)->buff_given),
                     expected_credit(2000.0, 1.05) + 200.0, 2.0);

    // A status never seen applied has nothing to go on.
    const uint64_t before = plugin.engine().accumulator_unlocked().find_stats(kMonk)->buff_given;
    statuses.by_actor[kSam] = {status(kBrotherhood, kMonk)};
    plugin.on_status_tick(kBoss, kSam, 1229, 4200, false);
    TEST_ASSERT_EQ(plugin.engine().accumulator_unlocked().find_stats(kMonk)->buff_given, before);
}

TEST_CASE(MeterRdps, DpsMetricSurvivesTheAutosave) {
    CombatPlugin plugin;
    plugin.initialize();
    plugin.set_dps_metric(DpsMetric::Rdps);

    hub::config::JsonValue saved(hub::config::JsonValue::ObjectType{});
    plugin.serialize_config(saved);
    TEST_ASSERT_EQ(saved["dps_metric"].as_int(-1), 1);

    CombatPlugin reloaded;
    reloaded.deserialize_config(saved);
    TEST_ASSERT_EQ(reloaded.dps_metric(), DpsMetric::Rdps);

    // Out of range reads as plain DPS.
    saved["dps_metric"] = hub::config::JsonValue(42);
    reloaded.deserialize_config(saved);
    TEST_ASSERT_EQ(reloaded.dps_metric(), DpsMetric::Dps);
}

TEST_CASE(MeterDecoder, StatusApplicationsLandOnTargetOrCaster) {
    hub::game::ActionEffectHeader header{};
    header.animation_target_id = kBoss;
    header.num_targets = 2;
    std::array<hub::game::ActionEffectEntry, 16> entries{};
    entries[0].effect_type = 0x0E; // on the target
    entries[0].value = kDokumori;
    entries[8].effect_type = 0x0F; // on the caster
    entries[8].value = 2513;
    const std::array<uint64_t, 2> targets{kBoss, kMonk};

    std::vector<decoder::StatusApplication> seen;
    const size_t count = decoder::decode_status_applications(kSam, header, entries.data(), targets.data(),
        [&](const decoder::StatusApplication& applied) { seen.push_back(applied); });
    TEST_ASSERT_EQ(count, 2u);
    TEST_ASSERT_EQ(seen[0].receiver_id, kBoss);
    TEST_ASSERT_EQ(seen[0].status_id, kDokumori);
    TEST_ASSERT_EQ(seen[1].receiver_id, kSam);
}

TEST_CASE(MeterGameData, RaidBuffTableMatchesTheStatusSheet) {
    // Ids drift between patches; the names pin each one to what the table means.
    const std::unordered_map<uint32_t, std::string_view> expected{
        {141, "Battle Voice"}, {786, "Battle Litany"}, {1185, "Brotherhood"}, {1221, "Chain Stratagem"},
        {1297, "Embolden"}, {1821, "Standard Finish"}, {1822, "Technical Finish"}, {1825, "Devilment"},
        {1878, "Divination"}, {2105, "Standard Finish"}, {2216, "The Wanderer's Minuet"},
        {2217, "Mage's Ballad"}, {2218, "Army's Paeon"}, {2599, "Arcane Circle"}, {2703, "Searing Light"},
        {2964, "Radiant Finale"}, {3685, "Starry Muse"}, {3849, "Dokumori"}, {3887, "The Balance"},
        {3889, "The Spear"},
    };
    TEST_ASSERT_EQ(hub::game::RAID_BUFFS.size(), expected.size());
    for (size_t i = 0; i < hub::game::RAID_BUFFS.size(); ++i) {
        const auto& buff = hub::game::RAID_BUFFS[i];
        if (i > 0) TEST_ASSERT(hub::game::RAID_BUFFS[i - 1].status_id < buff.status_id);
        TEST_ASSERT_EQ(hub::game::status_sheet_name(buff.status_id), expected.at(buff.status_id));
        TEST_ASSERT(hub::game::find_raid_buff(buff.status_id) == &buff);
        TEST_ASSERT(is_attribution_status(buff.status_id));
    }
    TEST_ASSERT(hub::game::find_raid_buff(49) == nullptr); // Medicated is personal

    TEST_ASSERT_EQ(hub::game::find_dance_finish(16196)->permille, 50u);
    TEST_ASSERT(hub::game::find_dance_finish(16004) == nullptr); // no steps, no buff
}

TEST_CASE(MeterGameData, GuaranteedHitsComeFromTheSheets) {
    using hub::game::GuaranteedHit;
    TEST_ASSERT_EQ(hub::game::guaranteed_hit_of_action(7487), GuaranteedHit::Crit);           // Midare Setsugekka
    TEST_ASSERT_EQ(hub::game::guaranteed_hit_of_action(16465), GuaranteedHit::CritDirectHit); // Inner Chaos
    TEST_ASSERT_EQ(hub::game::guaranteed_hit_of_action(31), GuaranteedHit::None);             // Heavy Swing
    // The buffs that grant a guarantee are not attacks themselves.
    TEST_ASSERT_EQ(hub::game::guaranteed_hit_of_action(83), GuaranteedHit::None);             // Life Surge
    TEST_ASSERT_EQ(hub::game::guaranteed_hit_of_status(851), GuaranteedHit::CritDirectHit);   // Reassembled
    TEST_ASSERT(hub::game::is_form_bonus_crit_action(36945));                                 // Leaping Opo
    TEST_ASSERT(hub::game::is_form_bonus_status(107));                                        // Opo-opo Form
    TEST_ASSERT(hub::game::is_gcd_action(31));
    TEST_ASSERT_FALSE(hub::game::is_gcd_action(38));                                          // Berserk
}
