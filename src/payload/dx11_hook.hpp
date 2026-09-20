#pragma once

#include <atomic>
#include <cstdint>

namespace hub::payload {

/**
 * @brief Manages DirectX 11 SwapChain (Present & ResizeBuffers) detours
 * for native in-process overlay rendering directly onto Final Fantasy XIV's backbuffer.
 */
class Dx11Hook {
public:
    static Dx11Hook& instance() noexcept;

    /// Locates IDXGISwapChain vtable and installs Present & ResizeBuffers detours
    bool install();

    /// Safely releases render targets and sets shutdown passthrough flag
    void uninstall();

    [[nodiscard]] bool is_installed() const noexcept { return m_installed.load(); }
    [[nodiscard]] const char* last_error() const noexcept { return m_last_error; }
    [[nodiscard]] void* game_hwnd() const noexcept { return m_game_hwnd; }
    [[nodiscard]] static bool is_shutting_down() noexcept;

private:
    Dx11Hook() = default;
    // Runs from the loader during DLL_PROCESS_DETACH, where releasing D3D and
    // shutting ImGui down would fault.
    ~Dx11Hook() { if (!is_shutting_down()) { uninstall(); } }
    Dx11Hook(const Dx11Hook&) = delete;
    Dx11Hook& operator=(const Dx11Hook&) = delete;

    std::atomic<bool> m_installed{false};
    const char* m_last_error{"OK"};
    void* m_game_hwnd{nullptr};
};

} // namespace hub::payload
