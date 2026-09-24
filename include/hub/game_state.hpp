#pragma once

#include <atomic>
#include <cstdint>

namespace hub {

/// Coarse game conditions overlays can gate their visibility on. Composed from
/// the client's Conditions flag array; several raw flags fold into one bit here
/// because the game splits the same player-visible state across duplicates.
enum class GameStateFlag : uint32_t {
    None       = 0,
    /// The source resolved its address. Absent means "unknown", not "false".
    Valid      = 1u << 0,
    InCombat   = 1u << 1,
    InCutscene = 1u << 2,
    InDuty     = 1u << 3,
    Occupied   = 1u << 4, ///< Event, dialog, trade or summoning bell is up
    Loading    = 1u << 5, ///< Zoning, logging out, character creation
    InPvP      = 1u << 6,
    /// No character in the world: title screen, data center or character select.
    /// Read from the local player id, not the Conditions array, so Valid says
    /// nothing about it.
    InLobby    = 1u << 7,
};

[[nodiscard]] constexpr uint32_t to_bits(GameStateFlag f) noexcept {
    return static_cast<uint32_t>(f);
}

[[nodiscard]] constexpr bool has_flag(uint32_t bits, GameStateFlag f) noexcept {
    return (bits & to_bits(f)) != 0;
}

/// Published by the payload's polling thread, read by the render thread.
///
/// Combat is tracked separately because it has two independent sources: the
/// Conditions array, and the combat meter's packet flag, which keeps working
/// when a signature breaks after a patch. Keeping them in their own words means
/// neither writer can clobber the other regardless of tick ordering. The lobby
/// has a word of its own for the same reason.
class GameStateProvider {
public:
    /// Called by GameStateReader with the flags it composed from the client.
    void publish(uint32_t flags) noexcept {
        m_flags.store(flags, std::memory_order_relaxed);
    }

    /// Called by the combat meter with the packet-derived combat state.
    void set_packet_combat(bool in_combat) noexcept {
        m_packet_combat.store(in_combat, std::memory_order_relaxed);
    }

    /// Called by the payload with ObjectReader::in_lobby().
    void set_in_lobby(bool in_lobby) noexcept {
        m_in_lobby.store(in_lobby, std::memory_order_relaxed);
    }

    [[nodiscard]] uint32_t flags() const noexcept {
        uint32_t bits = m_flags.load(std::memory_order_relaxed);
        if (m_packet_combat.load(std::memory_order_relaxed)) {
            bits |= to_bits(GameStateFlag::InCombat);
        }
        if (m_in_lobby.load(std::memory_order_relaxed)) {
            bits |= to_bits(GameStateFlag::InLobby);
        }
        return bits;
    }

    [[nodiscard]] bool has(GameStateFlag f) const noexcept {
        return has_flag(flags(), f);
    }

private:
    std::atomic<uint32_t> m_flags{0};
    std::atomic<bool> m_packet_combat{false};
    std::atomic<bool> m_in_lobby{false};
};

} // namespace hub
