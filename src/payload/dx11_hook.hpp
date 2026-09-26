#pragma once

#include <atomic>
#include <chrono>
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

    /// Sets the shutdown passthrough flag, waits for in-flight detour calls to
    /// leave, then releases render targets and shuts ImGui down. If a call is
    /// still inside after the timeout, leaks them instead of freeing under it.
    void uninstall();

    [[nodiscard]] bool is_installed() const noexcept { return m_installed.load(); }
    [[nodiscard]] const char* last_error() const noexcept { return m_last_error; }
    [[nodiscard]] void* game_hwnd() const noexcept { return m_game_hwnd; }

    /// True once the payload is unloading, the game window is gone for good, or
    /// the process is exiting. The one exit signal for Present, ResizeBuffers,
    /// WndProc and the orchestration loop.
    [[nodiscard]] static bool is_shutting_down() noexcept;

    /// The game is going away: its window was destroyed, or the Windows session
    /// is ending. Latches, since neither comes back.
    static void mark_game_exiting() noexcept;

    /// Counts a detour call, on any game thread, that reaches ImGui or the D3D
    /// objects this class owns, so uninstall() can wait for it to leave. Lives
    /// in the detour body, never inside a __try leaf (MSVC C2712). Take the scope
    /// first, then check is_shutting_down(): uninstall() sets the flag first and
    /// then reads the count, so one of the two always sees the other.
    class CallScope {
    public:
        CallScope() noexcept { s_in_flight.fetch_add(1); }
        ~CallScope() { s_in_flight.fetch_sub(1); }
        CallScope(const CallScope&) = delete;
        CallScope& operator=(const CallScope&) = delete;
    };

    /// Waits until no CallScope is alive, or the timeout passes. Returns whether
    /// it drained.
    [[nodiscard]] static bool drain_in_flight(std::chrono::milliseconds timeout);
    [[nodiscard]] static int32_t in_flight_calls() noexcept { return s_in_flight.load(); }

private:
    Dx11Hook() = default;
    // Runs from the loader during DLL_PROCESS_DETACH, where releasing D3D and
    // shutting ImGui down would fault.
    ~Dx11Hook() { if (!is_shutting_down()) { uninstall(); } }
    Dx11Hook(const Dx11Hook&) = delete;
    Dx11Hook& operator=(const Dx11Hook&) = delete;

    inline static std::atomic<int32_t> s_in_flight{0};

    std::atomic<bool> m_installed{false};
    const char* m_last_error{"OK"};
    void* m_game_hwnd{nullptr};
};

} // namespace hub::payload
