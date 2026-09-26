#include "app/app_state.hpp"
#include "app/connection_notifier.hpp"
#include "app/ui/app_frame.hpp"
#include "app/ui/theme.hpp"
#include "common/os/logger.hpp"
#include "common/os/auto_start.hpp"
#include "common/os/process_finder.hpp"
#include "common/os/single_instance.hpp"
#include "common/os/tray_manager.hpp"
#include "common/os/unique_handle.hpp"
#include "common/config/config_manager.hpp"
#include "hub/version.hpp"
#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>
#include <wrl/client.h>

#include "common/ui/imgui_guard.hpp"
#ifdef HAVE_IMGUI
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {

using Microsoft::WRL::ComPtr;

constexpr const wchar_t* kWindowClassName = L"FFXIVHubDesktopWindow";
constexpr DWORD kWindowStyle = WS_OVERLAPPEDWINDOW;
constexpr DWORD kWindowExStyle = WS_EX_APPWINDOW;

/// Owns a window, destroyed with DestroyWindow. The window's GWLP_USERDATA is
/// cleared first, since whatever it points to may already be half torn down.
struct WindowTraits {
    using pointer = HWND;
    static pointer empty() noexcept { return nullptr; }
    static bool is_valid(pointer hwnd) noexcept { return hwnd != nullptr; }
    static void close(pointer hwnd) noexcept {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        DestroyWindow(hwnd);
    }
};
using UniqueWindow = hub::os::BasicUniqueHandle<WindowTraits>;

/// The desktop window and the device that draws into it. MainWndProc reaches it
/// through GWLP_USERDATA. Members are released in reverse order: the render
/// target, swap chain, context and device, then the window.
struct MainWindow {
    UniqueWindow hwnd;
    /// DPI scale of the window, for WM_GETMINMAXINFO, which fires before the
    /// render loop has a chance to consult the theme.
    float dpi_scale{1.0f};
    bool minimized{false};
    bool running{true};

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap_chain;
    ComPtr<ID3D11RenderTargetView> render_target;
};

MainWindow* window_of(HWND hwnd) {
    return reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

/// Read per close rather than cached: the setting can be toggled while running,
/// and closing is rare enough that the lookup cost does not matter.
bool close_to_tray_enabled() {
    return hub::config::ConfigManager::instance().get("hub", "minimize_to_tray", true);
}

void create_render_target(MainWindow& window) {
    ComPtr<ID3D11Texture2D> back_buffer;
    window.swap_chain->GetBuffer(0, IID_PPV_ARGS(back_buffer.ReleaseAndGetAddressOf()));
    if (back_buffer) {
        window.device->CreateRenderTargetView(back_buffer.Get(), nullptr,
                                              window.render_target.ReleaseAndGetAddressOf());
    }
}

void resize_buffers(MainWindow& window, UINT width, UINT height) {
    window.render_target.Reset();
    window.swap_chain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    create_render_target(window);
}

bool create_device(MainWindow& window) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = window.hwnd.get();
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL feature_level;
    const D3D_FEATURE_LEVEL feature_levels[2] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };

    const HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, feature_levels, 2, D3D11_SDK_VERSION, &sd,
        window.swap_chain.ReleaseAndGetAddressOf(), window.device.ReleaseAndGetAddressOf(), &feature_level,
        window.context.ReleaseAndGetAddressOf());
    if (hr != S_OK) return false;

    create_render_target(window);
    return true;
}

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

// Undocumented uxtheme.dll ordinal used by Windows Terminal, VS Code, and Chromium to opt a
// process into dark-themed native controls (menus, scrollbars) on Windows 10 1809+/11.
// Resolved dynamically and never treated as a hard dependency: if the export is missing or the
// call fails, native menus simply keep the default light styling.
void EnableDarkModeForNativeMenus() {
    enum class PreferredAppMode { Default, AllowDark, ForceDark, ForceLight, Max };
    using SetPreferredAppModeFn = PreferredAppMode(WINAPI*)(PreferredAppMode);

    HMODULE ux_theme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux_theme) return;

    auto set_preferred_app_mode = reinterpret_cast<SetPreferredAppModeFn>(
        GetProcAddress(ux_theme, MAKEINTRESOURCEA(135)));
    if (set_preferred_app_mode) {
        set_preferred_app_mode(PreferredAppMode::AllowDark);
    }
}

LRESULT WINAPI MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

#ifdef HAVE_IMGUI
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
        return true;
    }
#endif

    // Null for the messages CreateWindowExW sends before WM_NCCREATE.
    MainWindow* window = window_of(hWnd);
    switch (msg) {
        case WM_GETMINMAXINFO: {
            // The layout reflows down to this size and no further, so the window
            // cannot be dragged to a width where cards and tables have nowhere to go.
            const float dpi_scale = window != nullptr ? window->dpi_scale : 1.0f;
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            RECT frame{ 0, 0,
                        static_cast<LONG>(hub::app::ui::metrics::WindowMinW * dpi_scale),
                        static_cast<LONG>(hub::app::ui::metrics::WindowMinH * dpi_scale) };
            // The frame at the window's own DPI, not the system's.
            AdjustWindowRectExForDpi(&frame, kWindowStyle, FALSE, kWindowExStyle, GetDpiForWindow(hWnd));
            mmi->ptMinTrackSize.x = frame.right - frame.left;
            mmi->ptMinTrackSize.y = frame.bottom - frame.top;
            return 0;
        }
        case WM_SIZE:
            if (window == nullptr) return 0;
            if (wParam == SIZE_MINIMIZED) {
                window->minimized = true;
                ShowWindow(hWnd, SW_HIDE);
                return 0;
            }
            window->minimized = false;
            if (window->device) {
                resize_buffers(*window, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam));
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) {
                return 0; // Disable ALT menu
            }
            break;
        case WM_CLOSE:
            if (close_to_tray_enabled()) {
                ShowWindow(hWnd, SW_HIDE);
                if (window != nullptr) window->minimized = true;
                return 0;
            }
            PostQuitMessage(0);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

/// The application icon (resource 101) at the large and small sizes.
struct WindowIcons {
    HICON large_icon{nullptr};
    HICON small_icon{nullptr};
};

WindowIcons load_icons(HINSTANCE instance) {
    WindowIcons icons;
    icons.large_icon = LoadIconW(instance, MAKEINTRESOURCEW(101));
    icons.small_icon = static_cast<HICON>(LoadImageW(
        instance,
        MAKEINTRESOURCEW(101),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR
    ));
    if (!icons.small_icon) {
        icons.small_icon = icons.large_icon;
    }
    if (!icons.large_icon) {
        icons.large_icon = LoadIconW(nullptr, IDI_APPLICATION);
        icons.small_icon = icons.large_icon;
    }
    return icons;
}

/// The desktop window's class, registered for this object's lifetime.
class WindowClass {
public:
    WindowClass(HINSTANCE instance, const WindowIcons& icons) : m_instance(instance) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_CLASSDC;
        wc.lpfnWndProc = MainWndProc;
        wc.hInstance = instance;
        wc.lpszClassName = kWindowClassName;
        wc.hIcon = icons.large_icon;
        wc.hIconSm = icons.small_icon;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        m_registered = RegisterClassExW(&wc) != 0;
    }
    ~WindowClass() {
        if (m_registered) UnregisterClassW(kWindowClassName, m_instance);
    }
    WindowClass(const WindowClass&) = delete;
    WindowClass& operator=(const WindowClass&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept { return m_registered; }

private:
    HINSTANCE m_instance;
    bool m_registered{false};
};

bool create_main_window(HINSTANCE instance, MainWindow& window) {
    window.hwnd.reset(CreateWindowExW(
        kWindowExStyle,
        kWindowClassName,
        L"FFXIV Hub",
        kWindowStyle,
        // Placeholder until the DPI is known: size_for_dpi() resizes and centres
        // it before the first show.
        CW_USEDEFAULT, CW_USEDEFAULT,
        static_cast<int>(hub::app::ui::metrics::WindowDefaultW),
        static_cast<int>(hub::app::ui::metrics::WindowDefaultH),
        nullptr, nullptr, instance, &window
    ));
    return static_cast<bool>(window.hwnd);
}

/// Reads the window's actual monitor DPI (the manifest opts into Per-Monitor V2
/// awareness) so the font and layout constants can be sized to match, instead of
/// rendering at a fixed 96-DPI pixel size on today's scaled displays.
///
/// CreateWindowExW sizes in physical pixels, so its size is only right at 100%;
/// at 150% the window opened below the layout's own minimum. Re-applied here in
/// scaled units before the first show, clamped to the work area and centred.
void size_for_dpi(MainWindow& window) {
    const HWND hwnd = window.hwnd.get();
    window.dpi_scale = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;

    RECT frame{ 0, 0,
                static_cast<LONG>(hub::app::ui::metrics::WindowDefaultW * window.dpi_scale),
                static_cast<LONG>(hub::app::ui::metrics::WindowDefaultH * window.dpi_scale) };
    AdjustWindowRectExForDpi(&frame, kWindowStyle, FALSE, kWindowExStyle, GetDpiForWindow(hwnd));
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
        const RECT& work = monitor.rcWork;
        const LONG width = std::min(frame.right - frame.left, work.right - work.left);
        const LONG height = std::min(frame.bottom - frame.top, work.bottom - work.top);
        SetWindowPos(hwnd, nullptr,
                     work.left + (work.right - work.left - width) / 2,
                     work.top + (work.bottom - work.top - height) / 2,
                     width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

/// The dark native title bar (a no-op before Windows 10 1809) and the icons.
void apply_window_chrome(const MainWindow& window, const WindowIcons& icons) {
    const HWND hwnd = window.hwnd.get();
    BOOL use_dark_titlebar = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &use_dark_titlebar, sizeof(use_dark_titlebar));

    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icons.large_icon));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icons.small_icon));
}

/// The tray menu's actions. Exit only ends the loop; the window is destroyed with
/// the rest of the teardown.
void wire_tray(hub::os::TrayManager& tray_manager, MainWindow& window) {
    tray_manager.set_on_show_window([&window]() {
        ShowWindow(window.hwnd.get(), SW_RESTORE);
        SetForegroundWindow(window.hwnd.get());
        window.minimized = false;
    });

    tray_manager.set_on_exit([&window]() {
        window.running = false;
    });

    tray_manager.set_auto_start(hub::os::AutoStart::is_enabled());
    tray_manager.set_on_toggle_auto_start([&tray_manager](bool enabled) {
        if (hub::os::AutoStart::set_enabled(enabled)) {
            tray_manager.set_auto_start(enabled);
            auto& cfg = hub::config::ConfigManager::instance();
            cfg.set("hub", "start_with_windows", hub::config::JsonValue(enabled));
            (void)cfg.save();
        } else {
            // The checkmark has to follow the registry, not the click, or it
            // silently claims a state that was never written.
            tray_manager.set_auto_start(hub::os::AutoStart::is_enabled());
            tray_manager.show_notification(
                "FFXIV Hub",
                "Could not change the Windows startup entry.");
        }
    });

    tray_manager.set_on_open_logs([]() {
        hub::os::Logger::open_log_folder();
    });

    tray_manager.set_on_open_config([]() {
        hub::os::Logger::open_config_file();
    });

    tray_manager.set_notifications_enabled(
        hub::config::ConfigManager::instance().get("hub", "show_notifications", true));
}

/// The ImGui context and its Win32 and DX11 backends, for this object's lifetime.
class ImGuiSession {
public:
    explicit ImGuiSession(const MainWindow& window) {
#ifdef HAVE_IMGUI
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr; // Window layout managed independently via JSON config (ConfigManager), not imgui.ini

        char windows_dir[MAX_PATH]{};
        GetWindowsDirectoryA(windows_dir, MAX_PATH);
        hub::app::ui::setup_fonts_and_theme(windows_dir, window.dpi_scale);

        ImGui_ImplWin32_Init(window.hwnd.get());
        ImGui_ImplDX11_Init(window.device.Get(), window.context.Get());
#else
        (void)window;
#endif
    }
    ~ImGuiSession() {
#ifdef HAVE_IMGUI
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
#endif
    }
    ImGuiSession(const ImGuiSession&) = delete;
    ImGuiSession& operator=(const ImGuiSession&) = delete;
};

void render_frame(MainWindow& window, hub::app::AppState& app_state, hub::app::ui::AppFrame& frame) {
#ifdef HAVE_IMGUI
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    frame.render(app_state);

    ImGui::Render();
    const ImVec4 canvas = ImGui::ColorConvertU32ToFloat4(hub::app::ui::colors::Canvas);
    const float clear_color[4] = { canvas.x, canvas.y, canvas.z, 1.0f };
    ID3D11RenderTargetView* const render_target = window.render_target.Get();
    window.context->OMSetRenderTargets(1, &render_target, nullptr);
    window.context->ClearRenderTargetView(render_target, clear_color);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    window.swap_chain->Present(1, 0); // VSync
#else
    (void)window;
    (void)app_state;
    (void)frame;
#endif
}

void run_frame_loop(MainWindow& window, hub::app::AppState& app_state, hub::os::TrayManager& tray_manager) {
    hub::app::ui::AppFrame frame;
    hub::app::ConnectionNotifier notifier;

    MSG msg{};
    while (window.running) {
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                window.running = false;
            }
        }
        if (!window.running) break;

        tray_manager.pump_messages();
        if (!window.running) break;
        app_state.update();

        for (const hub::app::TrayNotice& notice : notifier.update(
                 app_state.connection_state(), app_state.is_access_denied(), app_state.game_pid())) {
            tray_manager.show_notification(notice.title, notice.message);
        }
        tray_manager.set_status(app_state.connection_status_string());

        if (window.minimized) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        render_frame(window, app_state, frame);
    }
}

/// Opens the desktop window and runs it until the app exits. Everything it opens
/// is released on return, in reverse: ImGui, the device, the window, its class.
int run_desktop_window(HINSTANCE instance, hub::app::AppState& app_state, hub::os::TrayManager& tray_manager) {
    const WindowIcons icons = load_icons(instance);
    const WindowClass window_class(instance, icons);
    MainWindow window;
    if (!window_class || !create_main_window(instance, window) || !create_device(window)) {
        return 1;
    }
    size_for_dpi(window);
    apply_window_chrome(window, icons);
    wire_tray(tray_manager, window);

    const ImGuiSession imgui(window);
    ShowWindow(window.hwnd.get(), SW_SHOWDEFAULT);
    UpdateWindow(window.hwnd.get());

    run_frame_loop(window, app_state, tray_manager);
    // The window goes with this scope; the tray outlives it.
    tray_manager.set_on_show_window({});
    tray_manager.set_on_exit({});
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    hub::os::Logger::init();
    hub::os::Logger::info("Starting FFXIV Hub Desktop Manager v" HUB_VERSION_STRING "...");

    // Best-effort: opt this process into dark-themed native menus/controls before any window
    // or menu is created.
    EnableDarkModeForNativeMenus();

    // SeDebugPrivilege, to attach to a game running as Administrator.
    if (hub::os::ProcessFinder::enable_debug_privilege()) {
        hub::os::Logger::info("SeDebugPrivilege acquired successfully.");
    } else {
        hub::os::Logger::warn("Could not acquire SeDebugPrivilege. If FFXIV is running as Administrator, please run FFXIV Hub as Administrator.");
    }

    hub::os::SingleInstance single_instance("Local\\FFXIVHubSingleInstanceMutex");
    if (!single_instance.try_acquire()) {
        hub::os::Logger::info("Another instance is already running. Waking up existing instance and exiting.");
        single_instance.notify_existing_instance();
        return 0;
    }

    hub::app::AppState app_state;
    if (!app_state.initialize()) {
        hub::os::Logger::error("Failed to initialize AppState.");
        return 1;
    }

    hub::os::TrayManager tray_manager;
    tray_manager.initialize(single_instance.activation_message_id());

    if (const int code = run_desktop_window(hInstance, app_state, tray_manager); code != 0) {
        return code;
    }

    tray_manager.shutdown();
    app_state.shutdown();
    hub::os::Logger::shutdown();
    return 0;
}
#else
int main(int argc, char** argv) {
    std::cout << "FFXIV Hub Desktop Manager (CLI Mode for Non-Windows host)" << std::endl;
    hub::app::AppState app_state;
    app_state.initialize();
    std::cout << "AppState initialized. Registered plugins: " << app_state.registered_plugins().size() << std::endl;
    for (const auto& p : app_state.registered_plugins()) {
        std::cout << " - [" << static_cast<int>(p.id) << "] " << p.name << " v" << p.version << ": " << p.description << std::endl;
    }
    app_state.shutdown();
    return 0;
}
#endif
