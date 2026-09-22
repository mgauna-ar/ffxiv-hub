#include "meter/action_decoder.hpp"
#include <chrono>
#include <algorithm>

namespace hub::meter::decoder {

namespace {
    /// ActionEffectEntry::flags bits.
    constexpr uint8_t EFFECT_FLAG_EXTENDED_VALUE = 0x40; // high_byte holds bits 16-23 of the value
    constexpr uint8_t EFFECT_FLAG_ON_SOURCE = 0x80;      // lands on the source, e.g. a drain's self-heal

    /// ActionEffectEntry::hit_severity bits for damage.
    constexpr uint8_t SEVERITY_CRIT = 0x20;
    constexpr uint8_t SEVERITY_DIRECT_HIT = 0x40;
}

size_t decode_action_effects(
    uint32_t source_id,
    const game::ActionEffectHeader& header,
    const void* effect_data,
    const void* targets,
    uint64_t timestamp_us,
    ActionPacketCallback callback
) {
    if (effect_data == nullptr || !callback) {
        return 0;
    }

    const uint8_t raw_num_targets = header.num_targets;
    if (raw_num_targets == 0) {
        return 0;
    }

    const uint8_t num_targets = std::min<uint8_t>(
        raw_num_targets,
        static_cast<uint8_t>(game::definitions::MAX_TARGETS_PER_ACTION)
    );

    if (timestamp_us == 0) {
        timestamp_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()
            ).count()
        );
    }

    const uint32_t action_id = header.action_id;
    const auto* target_ids_ptr = static_cast<const uint64_t*>(targets);
    size_t packet_count = 0;

    for (uint8_t t = 0; t < num_targets; ++t) {
        // targets[] runs parallel to the effect blocks, slot 0 included. The
        // animation target is only a fallback: a self-centred AoE animates on the
        // caster, not on whoever its first block hit.
        uint64_t target_id = header.animation_target_id;
        if (target_ids_ptr != nullptr && target_ids_ptr[t] != 0) {
            target_id = target_ids_ptr[t];
        }

        const auto* entries = &reinterpret_cast<const game::ActionEffectEntry*>(
            effect_data
        )[t * game::definitions::MAX_EFFECT_ENTRIES_PER_TARGET];

        for (size_t i = 0; i < game::definitions::MAX_EFFECT_ENTRIES_PER_TARGET; ++i) {
            const auto& entry = entries[i];
            const uint8_t raw_type = entry.effect_type;
            if (raw_type == 0) {
                continue; // Unused entry
            }

            EffectType effect_type = EffectType::None;
            uint32_t damage = 0;
            uint32_t effective_heal = 0;
            uint16_t flags = HitFlags::None;

            uint32_t val = entry.value;
            if (entry.flags & EFFECT_FLAG_EXTENDED_VALUE) {
                val += static_cast<uint32_t>(entry.high_byte) << 16;
            }

            switch (raw_type) {
                case 0x01: // Miss
                    effect_type = EffectType::Miss;
                    flags |= HitFlags::Miss;
                    break;
                case 0x03: // Damage
                    effect_type = EffectType::Damage;
                    damage = val;
                    break;
                case 0x04: // Heal
                    effect_type = EffectType::Heal;
                    effective_heal = val;
                    break;
                case 0x05: // Blocked
                    effect_type = EffectType::Blocked;
                    damage = val;
                    flags |= HitFlags::Blocked;
                    break;
                case 0x06: // Parried
                    effect_type = EffectType::Parried;
                    damage = val;
                    flags |= HitFlags::Parried;
                    break;
                // 0x0A/0x0B are MP loss/gain, not status effects, and nothing here
                // books them; they fall through with the rest.
                default:
                    effect_type = EffectType::None;
                    break;
            }

            if (effect_type == EffectType::None) {
                continue;
            }

            if (entry.hit_severity & SEVERITY_CRIT) {
                flags |= HitFlags::Crit;
            }
            if (entry.hit_severity & SEVERITY_DIRECT_HIT) {
                flags |= HitFlags::DirectHit;
            }

            const HitSeverity severity = hit_flags_to_severity(flags);

            ipc::CombatActionPacket pkt{};
            pkt.source_id = source_id;
            // A heal carried in the target's block but landing on the caster (drains,
            // Bloodwhetting-style self-heals) must be measured against the caster's HP.
            pkt.target_id = (effect_type == EffectType::Heal && (entry.flags & EFFECT_FLAG_ON_SOURCE))
                ? source_id
                : target_id;
            pkt.action_id = action_id;
            pkt.damage = damage;
            pkt.effective_heal = effective_heal;
            pkt.overheal = 0;
            pkt.effect_type = static_cast<uint16_t>(effect_type);
            pkt.hit_flags = flags;
            pkt.severity = static_cast<uint8_t>(severity);
            pkt.timestamp_us = timestamp_us;

            callback(pkt);
            ++packet_count;
        }
    }

    return packet_count;
}

void apply_overheal(ipc::CombatActionPacket& packet, uint32_t current_hp, uint32_t max_hp) noexcept {
    if (static_cast<EffectType>(packet.effect_type) != EffectType::Heal || max_hp == 0 || current_hp > max_hp) {
        return;
    }
    const uint32_t heal = packet.effective_heal + packet.overheal;
    const uint32_t missing = max_hp - current_hp;
    packet.overheal = (heal > missing) ? heal - missing : 0;
    packet.effective_heal = heal - packet.overheal;
}

std::vector<ipc::CombatActionPacket> decode_action_effects(
    uint32_t source_id,
    const game::ActionEffectHeader& header,
    const void* effect_data,
    const void* targets,
    uint64_t timestamp_us
) {
    std::vector<ipc::CombatActionPacket> results;
    decode_action_effects(
        source_id,
        header,
        effect_data,
        targets,
        timestamp_us,
        [&](const ipc::CombatActionPacket& pkt) {
            results.push_back(pkt);
        }
    );
    return results;
}

} // namespace hub::meter::decoder
