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
    bool        is_64_bit{false};
    void*       handle{nullptr};        // Win32 HANDLE
    void*       window_handle{nullptr}; // Win32 HWND
    uint32_t    last_error{0};
};

/**
 * @brief Locates running game processes using Win32 Toolhelp32 snapshot and window enumeration.
 */
class ProcessFinder {
public:
    static bool enable_debug_privilege();

    [[nodiscard]] static std::optional<ProcessInfo> find_process(
        std::string_view process_name = game::definitions::DEFAULT_GAME_PROCESS_NAME
    );

    [[nodiscard]] static std::optional<ProcessInfo> find_process_by_pid(uint32_t pid);
    [[nodiscard]] static void* find_game_window(uint32_t pid);
    [[nodiscard]] static bool is_process_64_bit(void* process_handle);

    [[nodiscard]] static std::optional<ProcessInfo> wait_for_process(
        std::string_view process_name = game::definitions::DEFAULT_GAME_PROCESS_NAME,
        uint32_t timeout_seconds = 0
    );
};

} // namespace hub::os
