#include "app/app_state.hpp"
#include "app/ui/theme.hpp"
#include "app/ui/sidebar.hpp"
#include "app/ui/view_dashboard.hpp"
#include "app/ui/view_combat.hpp"
#include "app/ui/view_latency.hpp"
#include "app/ui/view_settings.hpp"
#include "common/os/logger.hpp"
#include "common/os/process_finder.hpp"
#include "common/os/single_instance.hpp"
#include "common/os/tray_manager.hpp"
#include "common/config/config_manager.hpp"
#include <iostream>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <dwmapi.h>

#if __has_include("third_party/imgui/imgui.h")
#include "third_party/imgui/imgui.h"
#include "third_party/imgui/backends/imgui_impl_win32.h"
#include "third_party/imgui/backends/imgui_impl_dx11.h"
#define HAVE_IMGUI 1
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace {
ID3D11Device* g_pd3dDevice = nullptr;
ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
IDXGISwapChain* g_pSwapChain = nullptr;
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;
bool g_window_minimized = false;

/// Read per close rather than cached: the setting can be toggled while running,
/// and closing is rare enough that the lookup cost does not matter.
bool close_to_tray_enabled() {
    auto& root = hub::config::ConfigManager::instance().root();
    if (!root.contains("hub") || !root["hub"].is_object()) return true;
    return root["hub"]["minimize_to_tray"].as_bool(true);
}
bool g_running = true;

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    if (pBackBuffer) {
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
        pBackBuffer->Release();
    }
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release();
        g_mainRenderTargetView = nullptr;
    }
}

bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = {
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_0,
    };

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        createDeviceFlags,
        featureLevelArray,
        2,
        D3D11_SDK_VERSION,
        &sd,
        &g_pSwapChain,
        &g_pd3dDevice,
        &featureLevel,
        &g_pd3dDeviceContext
    );

    if (hr != S_OK) return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
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
#ifdef HAVE_IMGUI
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) {
        return true;
    }
#endif

    switch (msg) {
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) {
                g_window_minimized = true;
                ShowWindow(hWnd, SW_HIDE);
                return 0;
            }
            g_window_minimized = false;
            if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED) {
                CleanupRenderTarget();
                g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
                CreateRenderTarget();
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
                g_window_minimized = true;
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

} // namespace
#endif

#ifdef _WIN32
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    hub::os::Logger::init();
    hub::os::Logger::info("Starting FFXIV Hub Desktop Manager v1.0.0...");

    // Best-effort: opt this process into dark-themed native menus/controls before any window
    // or menu is created.
    EnableDarkModeForNativeMenus();

    // Enable SeDebugPrivilege and SeSecurityPrivilege for game process attachment
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

    // Register Desktop Window Class with application icon (ID 101)
    HICON hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(101));
    HICON hIconSm = static_cast<HICON>(LoadImageW(
        hInstance,
        MAKEINTRESOURCEW(101),
        IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON),
        GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR
    ));
    if (!hIconSm) {
        hIconSm = hIcon;
    }
    if (!hIcon) {
        hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
        hIconSm = hIcon;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"FFXIVHubDesktopWindow";
    wc.hIcon = hIcon;
    wc.hIconSm = hIconSm;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_APPWINDOW,
        L"FFXIVHubDesktopWindow",
        L"FFXIV Hub - Dawntrail 7.x",
        WS_OVERLAPPEDWINDOW,
        100, 100, 1020, 680,
        nullptr, nullptr, hInstance, nullptr
    );

    if (!hwnd || !CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(L"FFXIVHubDesktopWindow", hInstance);
        return 1;
    }

    // Read the window's actual monitor DPI (the manifest opts into Per-Monitor V2 awareness)
    // so the font and hardcoded layout constants can be sized to match, instead of rendering
    // at a fixed 96-DPI pixel size on today's scaled displays.
    const float dpi_scale = static_cast<float>(GetDpiForWindow(hwnd)) / 96.0f;

    // Best-effort: make the native titlebar match the app's dark theme instead of the default
    // light chrome. No-ops silently on pre-1809 Windows.
    BOOL use_dark_titlebar = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &use_dark_titlebar, sizeof(use_dark_titlebar));

    SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hIcon));
    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hIconSm));

    // Configure Tray Callbacks
    tray_manager.set_on_show_window([hwnd]() {
        ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        g_window_minimized = false;
    });

    tray_manager.set_on_exit([hwnd]() {
        g_running = false;
        DestroyWindow(hwnd);
    });

#ifdef HAVE_IMGUI
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // Window layout managed independently via JSON config (ConfigManager), not imgui.ini

    // Load the system's Segoe UI font at a real, DPI-scaled pixel size in place of ImGui's
    // built-in bitmap font (which renders at a fixed, tiny size regardless of display scale).
    char windows_dir[MAX_PATH]{};
    GetWindowsDirectoryA(windows_dir, MAX_PATH);
    const std::string regular_font_path = std::string(windows_dir) + "\\Fonts\\segoeui.ttf";
    const std::string bold_font_path = std::string(windows_dir) + "\\Fonts\\segoeuib.ttf";
    const float base_font_size = 16.0f * dpi_scale;

    if (!io.Fonts->AddFontFromFileTTF(regular_font_path.c_str(), base_font_size)) {
        hub::os::Logger::warn("Segoe UI not found; falling back to ImGui's built-in font.");
        io.Fonts->AddFontDefault();
    }
    hub::app::ui::set_bold_font(io.Fonts->AddFontFromFileTTF(bold_font_path.c_str(), base_font_size));

    hub::app::ui::set_ui_scale(dpi_scale);
    hub::app::ui::apply_slate_theme();
    ImGui::GetStyle().ScaleAllSizes(dpi_scale);

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
#endif

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    MSG msg{};
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                g_running = false;
            }
        }
        if (!g_running) break;

        tray_manager.pump_messages();
        app_state.update();
        tray_manager.set_game_connected(app_state.is_connected(), app_state.game_pid());

        if (g_window_minimized) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

#ifdef HAVE_IMGUI
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Fill entire window client area
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGuiWindowFlags root_flags = ImGuiWindowFlags_NoTitleBar |
                                      ImGuiWindowFlags_NoResize |
                                      ImGuiWindowFlags_NoMove |
                                      ImGuiWindowFlags_NoCollapse |
                                      ImGuiWindowFlags_NoBringToFrontOnFocus;

        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

        ImGui::Begin("##RootWindow", nullptr, root_flags);

        // 1. Sidebar Navigation
        hub::app::ui::render_sidebar(app_state);

        ImGui::SameLine();

        // 2. Main Content View Area
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 16.0f));
        ImGui::BeginChild("##MainContentViewArea", ImVec2(0.0f, 0.0f), false);

        switch (app_state.current_view()) {
            case hub::app::DesktopView::Dashboard:
                hub::app::ui::render_view_dashboard(app_state);
                break;
            case hub::app::DesktopView::CombatMeter:
                hub::app::ui::render_view_combat(app_state);
                break;
            case hub::app::DesktopView::LatencyMitigator:
                hub::app::ui::render_view_latency(app_state);
                break;
            case hub::app::DesktopView::Settings:
                hub::app::ui::render_view_settings(app_state);
                break;
        }

        ImGui::EndChild();
        ImGui::PopStyleVar(); // WindowPadding

        ImGui::End();
        ImGui::PopStyleVar(3);

        ImGui::Render();
        const float clear_color[4] = { 0.043f, 0.055f, 0.078f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        g_pSwapChain->Present(1, 0); // VSync
#endif
    }

#ifdef HAVE_IMGUI
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
#endif

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(L"FFXIVHubDesktopWindow", hInstance);

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
