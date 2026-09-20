#pragma once

#include "hub/game_definitions.hpp"
#include "common/ipc/protocol.hpp"
#include "meter/types.hpp"
#include <vector>
#include <functional>
#include <cstdint>
#include <cstddef>

namespace hub::meter::decoder {

using ActionPacketCallback = std::function<void(const ipc::CombatActionPacket&)>;

/**
 * @brief Decodes raw FFXIV ActionEffectHeader and ActionEffectEntry buffer into CombatActionPackets.
 *
 * @param source_id Entity ID of the action instigator.
 * @param header ActionEffectHeader containing action ID, target count, and primary target.
 * @param effect_data Pointer to contiguous array of ActionEffectEntry[8] records per target.
 * @param targets Pointer to array of uint64_t target IDs for secondary targets (or nullptr).
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

} // namespace hub::meter::decoder
