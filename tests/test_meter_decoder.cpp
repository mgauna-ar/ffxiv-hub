#include "test_framework.hpp"
#include "meter/types.hpp"
#include "meter/action_decoder.hpp"
#include <array>

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
