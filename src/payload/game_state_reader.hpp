#pragma once

#include "hub/game_definitions.hpp"
#include "hub/game_state.hpp"
#include <cstddef>
#include <cstdint>

namespace hub::payload {

/// Folds the client's raw Conditions flags into GameStateFlag bits. Several
/// raw flags map onto one bit because the game splits the same player-visible
/// state across duplicate entries. Kept out of the platform split so it is
/// testable without a game process.
///
/// @param conditions Raw flag array; entries are one byte each.
/// @param count Number of readable entries; indices past it are treated as 0.
[[nodiscard]] uint32_t compose_game_state_flags(const uint8_t* conditions, size_t count) noexcept;

/// SEH-protected reader for the game's Conditions singleton, polled from the
/// payload's orchestration loop and published for the render thread.
class GameStateReader {
public:
    bool initialize();
    void poll(GameStateProvider& out);

    [[nodiscard]] bool is_initialized() const noexcept { return m_conditions_addr != 0; }
    [[nodiscard]] const char* last_error() const noexcept { return m_last_error; }

private:
    [[maybe_unused]] uintptr_t m_conditions_addr{0};
    const char* m_last_error{"not initialized"};
};

} // namespace hub::payload
