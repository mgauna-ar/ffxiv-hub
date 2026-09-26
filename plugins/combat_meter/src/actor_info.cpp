#include "meter/actor_info.hpp"
#include "common/os/safe_memory.hpp"
#include "hub/game/entity.hpp"
#include <cstring>

namespace hub::meter {

ActorType actor_type_from_object_kind(game::ObjectKind object_kind, uint32_t owner_id) noexcept {
    if (object_kind == game::ObjectKind::Kind5 || owner_id != 0) return ActorType::Pet;
    if (object_kind == game::ObjectKind::Player) return ActorType::Player;
    return ActorType::Monster;
}

bool extract_actor_info(const game::CharacterObject& obj, ipc::ActorInfoPacket& out) noexcept {
    out.entity_id = obj.entity_id;
    if (!hub::game::is_real_entity_id(out.entity_id)) return false;

    // The game writes NO_ENTITY_ID rather than 0 when an actor has no owner.
    out.owner_id = (obj.owner_id != hub::game::NO_ENTITY_ID) ? obj.owner_id : 0;
    out.job_id = obj.class_job;
    out.max_hp = obj.max_hp;
    out.current_hp = obj.current_hp;
    out.actor_type = static_cast<uint8_t>(actor_type_from_object_kind(obj.object_kind, out.owner_id));

    std::memcpy(out.name, obj.name, sizeof(out.name) - 1);
    out.name[sizeof(out.name) - 1] = '\0';
    return true;
}

bool read_actor_info(const void* character, ipc::ActorInfoPacket& out) noexcept {
    game::CharacterObject copy;
    if (!hub::os::safe_read(character, copy)) return false;
    return extract_actor_info(copy, out);
}

} // namespace hub::meter
