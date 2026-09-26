#include "payload/wndproc_hook.hpp"
#include "payload/overlay_host.hpp"
#include "payload/dx11_hook.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "imgui.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace hub::payload {

namespace {

WNDPROC g_original_wndproc = nullptr;

// Hands the message to ImGui and returns true when an overlay consumed it. Takes
// a Dx11Hook::CallScope first, so uninstall() cannot destroy the ImGui context
// while this thread is inside it.
bool overlay_consumes(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    Dx11Hook::CallScope scope;
    if (Dx11Hook::is_shutting_down() || ImGui::GetCurrentContext() == nullptr) {
        return false;
    }

    // Forward message to ImGui Win32 backend
    ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);

    // Right Mouse Button is reserved for FFXIV camera rotation and targeting; never intercept
    if ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0) {
        return false;
    }

    const ImGuiIO& io = ImGui::GetIO();
    const bool want_capture = io.WantCaptureMouse;

    // Swallow mouse clicks and scrolling directed at active overlay windows
    switch (msg) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN:
        case WM_XBUTTONDBLCLK:
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL:
        case WM_LBUTTONUP:
        case WM_MBUTTONUP:
        case WM_XBUTTONUP:
            return want_capture;
        default:
            return false;
    }
}

LRESULT CALLBACK hooked_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    const WNDPROC orig = g_original_wndproc;

    // The window going away, or the Windows session ending, is final: raise the
    // exit signal Present, ResizeBuffers and the payload thread share, so no
    // further overlay frame is drawn. WM_CLOSE is not final - the game may ask
    // and the player cancel - so it changes nothing. Every message still reaches
    // the original WndProc, so FFXIV runs its own shutdown sequence.
    if (msg == WM_DESTROY || (msg == WM_ENDSESSION && wparam != FALSE)) {
        Dx11Hook::mark_game_exiting();
    }

    if (!orig) {
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    if (overlay_consumes(hwnd, msg, wparam, lparam)) {
        return 0;
    }

    return CallWindowProcW(orig, hwnd, msg, wparam, lparam);
}

} // namespace

WndProcHook& WndProcHook::instance() noexcept {
    static WndProcHook s_instance;
    return s_instance;
}

bool WndProcHook::install(void* hwnd) {
    if (m_installed.load() || !hwnd) return true;

    HWND target = static_cast<HWND>(hwnd);
    if (!IsWindow(target)) return false;

    g_original_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        target, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hooked_wndproc)
    ));

    m_installed.store(g_original_wndproc != nullptr);
    return m_installed.load();
}

void WndProcHook::uninstall() {
    // Non-destructive passthrough: never restore old WndProc pointer via SetWindowLongPtrW,
    // as doing so stomps chained hooks (ReShade/Dalamud/OBS).
    m_installed.store(false);
}

} // namespace hub::payload

#else // !_WIN32 - Cross-platform mock implementation

namespace hub::payload {

WndProcHook& WndProcHook::instance() noexcept {
    static WndProcHook s_instance;
    return s_instance;
}

bool WndProcHook::install(void*) {
    m_installed.store(true);
    return true;
}

void WndProcHook::uninstall() {
    m_installed.store(false);
}

} // namespace hub::payload

#endif
