#pragma once

#include "hub/game_definitions.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <optional>

namespace hub::os {

struct ProcessInfo {
    uint32_t    pid{0};
    std::string name;
    void*       handle{nullptr};        // Win32 HANDLE
    uint32_t    last_error{0};
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

};

} // namespace hub::os
