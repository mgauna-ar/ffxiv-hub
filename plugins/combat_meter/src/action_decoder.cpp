#include "meter/action_decoder.hpp"
#include <chrono>
#include <algorithm>

namespace hub::meter::decoder {

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
        uint64_t target_id = header.animation_target_id;
        if (t > 0 && target_ids_ptr != nullptr) {
            const uint64_t next_id = target_ids_ptr[t];
            if (next_id != 0) {
                target_id = next_id;
            }
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

            // FFXIV client packs 24-bit values: if flags & 0x40, high_byte is shifted left by 16
            uint32_t val = entry.value;
            if (entry.flags & 0x40) {
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
                case 0x0A: // Buff
                    effect_type = EffectType::Buff;
                    break;
                case 0x0B: // Debuff
                    effect_type = EffectType::Debuff;
                    break;
                default:
                    effect_type = EffectType::None;
                    break;
            }

            if (effect_type == EffectType::None) {
                continue;
            }

            // In FFXIV client, bit 5 (0x20) is Critical and bit 6 (0x40) is Direct Hit.
            // Support both standard FFXIV flags (0x20/0x40) and normalized mock flags (0x01/0x02).
            if ((entry.hit_severity & 0x20) || (entry.hit_severity & 0x01)) {
                flags |= HitFlags::Crit;
            }
            if ((entry.hit_severity & 0x40) || (entry.hit_severity & 0x02)) {
                flags |= HitFlags::DirectHit;
            }

            const HitSeverity severity = hit_flags_to_severity(flags);

            ipc::CombatActionPacket pkt{};
            pkt.source_id = source_id;
            pkt.target_id = target_id;
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
