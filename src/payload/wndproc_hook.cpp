#include "payload/wndproc_hook.hpp"
#include "payload/overlay_host.hpp"

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
std::atomic<bool> g_game_exiting{false};

LRESULT CALLBACK hooked_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    const WNDPROC orig = g_original_wndproc;

    // 1. Detect game window close / destruction / quit / session end:
    // Mark shutdown immediately so no further DX11 overlay rendering occurs.
    // Forward directly to the original WndProc via CallWindowProcW so FFXIV can execute
    // its graceful shutdown sequence. Never call DefWindowProcW for WM_CLOSE!
    if (msg == WM_CLOSE || msg == WM_DESTROY || msg == WM_QUIT || msg == WM_ENDSESSION) {
        g_game_exiting.store(true);
        if (orig) {
            return CallWindowProcW(orig, hwnd, msg, wparam, lparam);
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    if (g_game_exiting.load() || !orig) {
        if (orig) {
            return CallWindowProcW(orig, hwnd, msg, wparam, lparam);
        }
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    // Global hotkey Ctrl+\ toggles click-through mode across all overlays
    if (msg == WM_KEYDOWN && wparam == VK_OEM_5 && (GetKeyState(VK_CONTROL) & 0x8000)) {
        WndProcHook::instance().toggle_click_through();
        return 0;
    }

    if (!WndProcHook::instance().click_through() && ImGui::GetCurrentContext() != nullptr) {
        // Forward message to ImGui Win32 backend
        ImGui_ImplWin32_WndProcHandler(hwnd, msg, wparam, lparam);

        // Right Mouse Button is reserved for FFXIV camera rotation and targeting; never intercept
        if ((GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0) {
            return CallWindowProcW(orig, hwnd, msg, wparam, lparam);
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
                if (want_capture) {
                    return 0;
                }
                break;
            case WM_LBUTTONUP:
            case WM_MBUTTONUP:
            case WM_XBUTTONUP:
                if (want_capture) {
                    return 0;
                }
                break;
            default:
                break;
        }
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

    m_game_hwnd = hwnd;
    g_original_wndproc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        target, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(hooked_wndproc)
    ));
    m_original_wndproc = reinterpret_cast<void*>(g_original_wndproc);

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

bool WndProcHook::install(void* hwnd) {
    m_game_hwnd = hwnd;
    m_installed.store(true);
    return true;
}

void WndProcHook::uninstall() {
    m_installed.store(false);
}

} // namespace hub::payload

#endif
