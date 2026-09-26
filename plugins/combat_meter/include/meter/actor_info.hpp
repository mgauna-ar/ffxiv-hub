#pragma once

#include "common/ipc/protocol.hpp"
#include "hub/game_definitions.hpp"
#include "meter/types.hpp"
#include <cstdint>

namespace hub::meter {

/// game::ObjectKind does not line up with ActorType, so only Player would survive a
/// direct cast. Anything with an owner is a pet; no object kind makes one (kind 5 is
/// an aetheryte). `owner_id` is already normalized: 0 for none.
[[nodiscard]] ActorType actor_type_from_object_kind(game::ObjectKind object_kind, uint32_t owner_id) noexcept;

/// The one Character-to-ActorInfo extraction, shared by the payload's object reader
/// and the meter's own source read. Works on a copy the caller owns: owner ids of
/// NO_ENTITY_ID become 0, the name is capped to what the packet carries, and the
/// actor type comes from actor_type_from_object_kind. False for an object whose id is
/// not a real entity id.
[[nodiscard]] bool extract_actor_info(const game::CharacterObject& obj, ipc::ActorInfoPacket& out) noexcept;

/// extract_actor_info on a Character the game owns, copied out first through
/// hub::os::safe_read so a stale pointer fails rather than faults. False for null.
[[nodiscard]] bool read_actor_info(const void* character, ipc::ActorInfoPacket& out) noexcept;

} // namespace hub::meter
