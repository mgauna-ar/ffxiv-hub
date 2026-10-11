#pragma once

#include "hub/game_definitions.hpp"
#include "common/ipc/protocol.hpp"
#include "meter/types.hpp"
#include <vector>
#include <functional>
#include <optional>
#include <cstdint>
#include <cstddef>

namespace hub::meter::decoder {

/// ActionEffectHeader::action_type of an Action row; items and others use other values.
inline constexpr uint8_t ACTION_TYPE_ACTION = 1;

using ActionPacketCallback = std::function<void(const ipc::CombatActionPacket&)>;

/**
 * @brief Decodes raw FFXIV ActionEffectHeader and ActionEffectEntry buffer into CombatActionPackets.
 *
 * Each entry is dealt and received as the client runs it: by the caster unless its flags
 * say the target dealt it (a reflect), and on the block's target unless they say it
 * landed on the caster (a drain). One the target dealt carries HitFlags::ByTarget. A
 * block whose target is the placeholder id is skipped, as the client skips it.
 *
 * @param source_id Entity ID of the action instigator.
 * @param header ActionEffectHeader containing action ID, target count, and primary target.
 * @param effect_data Pointer to contiguous array of ActionEffectEntry[8] records per target.
 * @param targets Pointer to array of uint64_t target IDs, one per effect block (or nullptr,
 *                which falls back to the header's animation target).
 * @param timestamp_us Combat timestamp in microseconds (0 to auto-generate).
 * @param callback Invoked for each valid non-empty CombatActionPacket decoded.
 * @return Number of combat action packets dispatched.
 */
size_t decode_action_effects(
    uint32_t source_id,
    const game::ActionEffectHeader& header,
    const void* effect_data,
    const void* targets,
    uint64_t timestamp_us,
    ActionPacketCallback callback
);

/**
 * @brief Helper that decodes into a std::vector<ipc::CombatActionPacket>.
 */
[[nodiscard]] std::vector<ipc::CombatActionPacket> decode_action_effects(
    uint32_t source_id,
    const game::ActionEffectHeader& header,
    const void* effect_data,
    const void* targets,
    uint64_t timestamp_us = 0
);

/// A status an action applied: effect kind 14 lands on the entry's receiver, 15 on its
/// dealer. Both follow the entry's flags, as for damage and heals.
struct StatusApplication {
    uint32_t dealer_id{0};
    uint32_t receiver_id{0};
    uint16_t status_id{0};
};
using StatusApplicationCallback = std::function<void(const StatusApplication&)>;

/**
 * @brief Walks the same effect blocks as decode_action_effects for the statuses the
 * action applied, the entries the client prints as "gains the effect of".
 * @return Number of applications dispatched.
 */
size_t decode_status_applications(
    uint32_t source_id,
    const game::ActionEffectHeader& header,
    const void* effect_data,
    const void* targets,
    StatusApplicationCallback callback
);

/**
 * @brief The button press behind an ActionEffect, one per effect however many targets it
 * hit. Nothing for an item, an auto-attack, or an effect the game fired by itself (a
 * Kardia heal); a Limit Break counts.
 */
[[nodiscard]] std::optional<ipc::CastPacket> decode_cast(
    uint32_t source_id,
    const game::ActionEffectHeader& header,
    uint64_t timestamp_us
) noexcept;

/**
 * @brief Splits a decoded heal into what landed and what overhealed.
 *
 * `current_hp` and `max_hp` are the target's as they stood before the heal applied.
 * Anything but a heal, or a target whose HP is unknown (max_hp 0), is left alone.
 */
void apply_overheal(ipc::CombatActionPacket& packet, uint32_t current_hp, uint32_t max_hp) noexcept;

/**
 * @brief Splits a HoT tick the same way: `damage_or_heal` keeps the full tick, and
 * `overheal` gets the part the target had no room for.
 */
void apply_overheal(ipc::StatusTickPacket& tick, uint32_t current_hp, uint32_t max_hp) noexcept;

} // namespace hub::meter::decoder
