#pragma once

#include "meter/types.hpp"
#include "common/ipc/protocol.hpp"
#include "hub/game_definitions.hpp"
#include <cstdint>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace hub::meter {

/// One actor's HP and status slots, as the payload read them this pass.
struct ActorVitals {
    EntityId entity{0};
    uint32_t hp{0};
    uint32_t max_hp{0};
    /// Party members and the local player: deaths and raises are reported.
    bool track_life{false};
    /// A tracked enemy: only statuses from friendly sources are kept.
    bool is_enemy{false};
    /// False when the status list could not be read this pass; the last one stands.
    bool statuses_read{false};
    /// False for the party list's copy, which carries no timers or sources.
    bool status_detail{true};
    uint8_t count{0};
    ipc::CombatStatusEntry entries[game::definitions::MAX_STATUS_SLOTS]{};
};

/// What the payload last published per actor, so a pass only sends what changed.
class VitalsTracker {
public:
    /// A timer longer than its countdown predicts by this much was refreshed.
    static constexpr double kRefreshSlackS = 0.5;

    /// Death or raise since the last pass. The first sighting of an actor is silent:
    /// nothing is known about what came before it.
    [[nodiscard]] std::optional<LifeEventKind> observe_life(const ActorVitals& vitals);

    /// True when `list` differs from what was last sent for its actor: another set
    /// of statuses, other stacks or detail, or a refreshed timer. A true result
    /// records `list` as sent.
    [[nodiscard]] bool status_list_changed(const ipc::StatusListPacket& list);

    /// Forgets every actor not in `seen`. Returns those that still had statuses
    /// published, each needing one empty list so the listener clears them.
    [[nodiscard]] std::vector<EntityId> retain(std::span<const EntityId> seen);

    /// Sends every list again on the next pass, for a listener that just connected.
    void invalidate();

    [[nodiscard]] size_t tracked_count() const noexcept { return m_actors.size(); }

private:
    struct Tracked {
        bool life_known{false};
        bool alive{true};
        bool list_sent{false};
        ipc::StatusListPacket last_list{};
    };
    std::unordered_map<EntityId, Tracked> m_actors;
};

} // namespace hub::meter
