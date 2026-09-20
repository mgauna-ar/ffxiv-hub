#pragma once

#include "meter/types.hpp"
#include "common/ipc/protocol.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <optional>

namespace hub::meter {

struct Combatant {
    EntityId entity_id{0};
    EntityId owner_id{0};
    std::string name;
    Job job{Job::None};
    Role role{Role::None};
    ActorType actor_type{ActorType::Unknown};
    uint32_t max_hp{0};
    uint32_t current_hp{0};
    uint16_t world_id{0};
    bool is_pet{false};
    bool is_party_member{false};
    bool is_local_player{false};
};

class CombatantRegistry {
public:
    CombatantRegistry() = default;

    /// Registers or updates an actor from an IPC ActorInfoPacket.
    void register_actor(const ipc::ActorInfoPacket& packet);

    /// Programmatically registers or updates an actor.
    Combatant& register_actor(
        EntityId entity_id,
        std::string name,
        Job job = Job::None,
        EntityId owner_id = 0,
        ActorType actor_type = ActorType::Unknown,
        uint32_t max_hp = 0,
        uint32_t current_hp = 0
    );

    /// Links a pet to its summoner or owner.
    void set_pet_owner(EntityId pet_id, EntityId owner_id);

    /// Resolves the ultimate owner of an entity. Returns entity_id if not a pet.
    [[nodiscard]] EntityId resolve_owner(EntityId entity_id) const;

    /// Checks if an entity is registered as a pet or has a registered owner.
    [[nodiscard]] bool is_pet(EntityId entity_id) const;

    /// Looks up a combatant by entity ID.
    [[nodiscard]] const Combatant* find_actor(EntityId entity_id) const;
    [[nodiscard]] Combatant* find_actor_mut(EntityId entity_id);

    /// Gets an existing combatant or creates a default one.
    [[nodiscard]] Combatant& get_or_create(EntityId entity_id, std::string_view default_name = "");

    /// Synchronizes the active party list from an IPC PartySyncPacket.
    void sync_party(const ipc::PartySyncPacket& packet);

    /// Manually sets party member entity IDs.
    void set_party_members(const std::vector<EntityId>& member_ids);

    /// Returns true if the entity is registered in the current party list.
    [[nodiscard]] bool is_party_member(EntityId entity_id) const;

    /// Returns true if entity is a friendly player, pet, or party member.
    [[nodiscard]] bool is_friendly(EntityId entity_id) const;

    /// Returns the active list of party member entity IDs.
    [[nodiscard]] const std::vector<EntityId>& party_members() const noexcept {
        return m_party_members;
    }

    /// Returns the number of tracked party members.
    [[nodiscard]] size_t party_size() const noexcept {
        return m_party_members.size();
    }

    /// Local player designation.
    void set_local_player(EntityId entity_id);
    [[nodiscard]] EntityId local_player_id() const noexcept {
        return m_local_player_id;
    }

    /// Updates HP status and alive state.
    void update_hp(EntityId entity_id, uint32_t current_hp, uint32_t max_hp = 0);

    /// Checks if all known party members are dead (current_hp == 0).
    [[nodiscard]] bool is_party_wiped() const;

    /// Total number of tracked actors.
    [[nodiscard]] size_t actor_count() const noexcept {
        return m_actors.size();
    }

    /// Read-only access to all registered actors.
    [[nodiscard]] const std::unordered_map<EntityId, Combatant>& all_actors() const noexcept {
        return m_actors;
    }

    /// Clears all cached actors and mappings.
    void clear();

    /// Known pet name recognition and job inference.
    [[nodiscard]] static bool is_known_pet_name(std::string_view name);
    [[nodiscard]] static Job infer_pet_job(std::string_view name);

private:
    std::unordered_map<EntityId, Combatant> m_actors;
    std::unordered_map<EntityId, EntityId> m_pet_to_owner;
    std::vector<EntityId> m_party_members;
    EntityId m_local_player_id{0};
};

} // namespace hub::meter
