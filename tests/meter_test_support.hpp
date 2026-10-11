#pragma once

// Helpers shared by more than one of the tests/test_meter_*.cpp files. A helper
// only one file uses stays in that file.

#include "hub/game_state.hpp"
#include "common/ipc/protocol.hpp"
#include "meter/combat_plugin.hpp"
#include "meter/encounter_engine.hpp"
#include "meter/types.hpp"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace hub::test::meter_support {

// What the payload reads from the Conditions array: Valid, plus InCombat while the
// game has the local player in combat.
inline constexpr uint32_t kOutOfGameCombat = hub::to_bits(hub::GameStateFlag::Valid);
inline constexpr uint32_t kInGameCombat = kOutOfGameCombat | hub::to_bits(hub::GameStateFlag::InCombat);

/// Opens a pull with one hit at `at` and ends it by hand two seconds later.
inline void archive_pull(hub::meter::EncounterEngine& engine, std::chrono::steady_clock::time_point at) {
    hub::ipc::CombatActionPacket act{};
    act.source_id = 1;
    act.damage = 4200;
    act.effect_type = static_cast<uint16_t>(hub::meter::EffectType::Damage);
    engine.process_action(act, at);
    engine.end_encounter(hub::meter::EncounterEndReason::Manual, at + std::chrono::seconds(2));
}

inline hub::ipc::ActorInfoPacket party_actor(uint32_t id, hub::meter::Job job, uint32_t current_hp, uint32_t max_hp) {
    hub::ipc::ActorInfoPacket actor{};
    actor.entity_id = id;
    actor.job_id = static_cast<uint32_t>(job);
    actor.current_hp = current_hp;
    actor.max_hp = max_hp;
    actor.actor_type = static_cast<uint8_t>(hub::meter::ActorType::Player);
    return actor;
}

/// One status as the payload's reader copies it out of a StatusManager.
inline hub::ipc::CombatStatusEntry status(uint16_t id, hub::meter::EntityId source, uint16_t param = 0) {
    hub::ipc::CombatStatusEntry entry{};
    entry.status_id = id;
    entry.param = param;
    entry.remaining_s = 10.0f;
    entry.source_id = source;
    return entry;
}

/// Statuses per actor, served the way the payload's reader serves them.
struct FakeStatuses {
    std::unordered_map<uint32_t, std::vector<hub::ipc::CombatStatusEntry>> by_actor;

    hub::meter::CombatPlugin::StatusReader reader() {
        return [this](uint32_t id, const void*, std::span<hub::ipc::CombatStatusEntry> out) -> std::optional<size_t> {
            const auto it = by_actor.find(id);
            if (it == by_actor.end()) return size_t{0};
            const size_t n = std::min(out.size(), it->second.size());
            std::copy_n(it->second.begin(), n, out.begin());
            return n;
        };
    }
};

} // namespace hub::test::meter_support
