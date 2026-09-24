#pragma once

#include "common/os/process_exit.hpp"
#include <atomic>

namespace hub::payload {

/**
 * @brief Subclasses FFXIV's game window procedure (WndProc) for non-interfering
 * mouse and keyboard input routing to in-game Dear ImGui overlays.
 */
class WndProcHook {
public:
    static WndProcHook& instance() noexcept;

    bool install(void* hwnd);
    void uninstall();

    [[nodiscard]] bool is_installed() const noexcept { return m_installed.load(); }
    [[nodiscard]] void* game_hwnd() const noexcept { return m_game_hwnd; }

private:
    WndProcHook() = default;
    ~WndProcHook() { if (!hub::os::is_process_exiting()) { uninstall(); } }
    WndProcHook(const WndProcHook&) = delete;
    WndProcHook& operator=(const WndProcHook&) = delete;

    std::atomic<bool> m_installed{false};
    void* m_game_hwnd{nullptr};
    [[maybe_unused]] void* m_original_wndproc{nullptr};
};

} // namespace hub::payload
