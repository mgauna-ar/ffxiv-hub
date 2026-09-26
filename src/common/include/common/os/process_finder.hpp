#pragma once

#include "hub/game_definitions.hpp"
#include "common/os/unique_handle.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <optional>

namespace hub::os {

struct ProcessInfo {
    uint32_t     pid{0};
    std::string  name;
    /// The game opened for injection; empty when OpenProcess refused.
    UniqueHandle handle;
    uint32_t     last_error{0};
};

/**
 * @brief Locates the running game process with a Win32 Toolhelp32 snapshot, falling
 * back to the FFXIVGAME window's owner.
 */
class ProcessFinder {
public:
    static bool enable_debug_privilege();

    [[nodiscard]] static std::optional<ProcessInfo> find_process(
        std::string_view process_name = game::definitions::DEFAULT_GAME_PROCESS_NAME
    );

    /// True when the main game window (FFXIVGAME) exists and belongs to `pid`.
    /// Always true on the mock.
    [[nodiscard]] static bool has_game_window(uint32_t pid);
};

} // namespace hub::os
