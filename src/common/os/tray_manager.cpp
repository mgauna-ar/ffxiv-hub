#include "common/os/tray_manager.hpp"
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

namespace {
constexpr UINT WM_HUB_TRAYICON = WM_USER + 100;
constexpr const wchar_t* TRAY_WINDOW_CLASS = L"FFXIV_Hub_TrayWindowClass";
}
#endif

namespace hub::os {

TrayManager::TrayManager() = default;

TrayManager::~TrayManager() {
    shutdown();
}

bool TrayManager::initialize(uint32_t activation_msg_id) {
    if (m_initialized) return true;
    m_activation_msg_id = activation_msg_id;

#ifdef _WIN32
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = reinterpret_cast<WNDPROC>(&TrayManager::static_wnd_proc);
    wc.hInstance = hInst;
    wc.lpszClassName = TRAY_WINDOW_CLASS;

    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        TRAY_WINDOW_CLASS,
        L"FFXIV Hub Tray Window",
        WS_POPUP,
        0, 0, 0, 0,
        nullptr,
        nullptr,
        hInst,
        this
    );

    if (!hwnd) {
        return false;
    }

    m_hwnd = hwnd;
    m_icon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_HUB_TRAYICON;
    nid.hIcon = static_cast<HICON>(m_icon);

    std::wstring tip = L"FFXIV Hub (Searching for game...)";
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);

    Shell_NotifyIconW(NIM_ADD, &nid);
#endif

    m_initialized = true;
    return true;
}

void TrayManager::shutdown() {
    if (!m_initialized) return;

#ifdef _WIN32
    if (m_hwnd) {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(NOTIFYICONDATAW);
        nid.hWnd = static_cast<HWND>(m_hwnd);
        nid.uID = 1;
        Shell_NotifyIconW(NIM_DELETE, &nid);

        DestroyWindow(static_cast<HWND>(m_hwnd));
        m_hwnd = nullptr;
    }

    UnregisterClassW(TRAY_WINDOW_CLASS, GetModuleHandleW(nullptr));
#endif

    m_initialized = false;
}

void TrayManager::set_game_connected(bool connected, uint32_t pid) {
    m_connected = connected;
    m_game_pid = pid;
    update_tray_icon();
}

std::string TrayManager::status_string() const {
    std::ostringstream ss;
    ss << "FFXIV Hub [";
    if (m_connected) {
        ss << "Connected (PID: " << m_game_pid << ")";
    } else {
        ss << "Searching for game...";
    }
    ss << "]";
    return ss.str();
}

void TrayManager::update_tray_icon() {
#ifdef _WIN32
    if (!m_initialized || !m_hwnd) return;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = static_cast<HWND>(m_hwnd);
    nid.uID = 1;
    nid.uFlags = NIF_TIP;

    std::string status = status_string();
    std::wstring wstatus(status.begin(), status.end());
    wcsncpy_s(nid.szTip, wstatus.c_str(), _TRUNCATE);

    Shell_NotifyIconW(NIM_MODIFY, &nid);
#endif
}

void TrayManager::show_notification(const std::string& title, const std::string& message) {
    if (!m_notifications_enabled) return;

#ifdef _WIN32
    if (!m_initialized || !m_hwnd) return;

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(NOTIFYICONDATAW);
    nid.hWnd = static_cast<HWND>(m_hwnd);
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;

    std::wstring wtitle(title.begin(), title.end());
    std::wstring wmsg(message.begin(), message.end());

    wcsncpy_s(nid.szInfoTitle, wtitle.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, wmsg.c_str(), _TRUNCATE);

    Shell_NotifyIconW(NIM_MODIFY, &nid);
#else
    (void)title;
    (void)message;
#endif
}

void TrayManager::handle_command(CommandId cmd) {
    switch (cmd) {
        case CommandId::ShowHubWindow:
            if (m_on_show_window) m_on_show_window();
            break;
        case CommandId::ToggleAutoStart:
            m_auto_start = !m_auto_start;
            if (m_on_toggle_auto_start) m_on_toggle_auto_start(m_auto_start);
            break;
        case CommandId::OpenConfig:
            if (m_on_open_config) m_on_open_config();
            break;
        case CommandId::OpenLogs:
            if (m_on_open_logs) m_on_open_logs();
            break;
        case CommandId::Exit:
            if (m_on_exit) m_on_exit();
            break;
    }
}

void TrayManager::pump_messages() {
#ifdef _WIN32
    MSG msg;
    while (PeekMessageW(&msg, static_cast<HWND>(m_hwnd), 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
#endif
}

#ifdef _WIN32
void TrayManager::show_context_menu() {
    POINT pt;
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    AppendMenuW(hMenu, MF_STRING, static_cast<UINT_PTR>(CommandId::ShowHubWindow), L"Open FFXIV Hub");
    SetMenuDefaultItem(hMenu, static_cast<UINT>(CommandId::ShowHubWindow), FALSE);

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    UINT autoStartFlags = MF_STRING | (m_auto_start ? MF_CHECKED : MF_UNCHECKED);
    AppendMenuW(hMenu, autoStartFlags, static_cast<UINT_PTR>(CommandId::ToggleAutoStart), L"Start with Windows");

    AppendMenuW(hMenu, MF_STRING, static_cast<UINT_PTR>(CommandId::OpenLogs), L"Open Logs Folder");
    AppendMenuW(hMenu, MF_STRING, static_cast<UINT_PTR>(CommandId::OpenConfig), L"Open Configuration File");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(hMenu, MF_STRING, static_cast<UINT_PTR>(CommandId::Exit), L"Exit");

    SetForegroundWindow(static_cast<HWND>(m_hwnd));
    TrackPopupMenuEx(hMenu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, static_cast<HWND>(m_hwnd), nullptr);
    PostMessageW(static_cast<HWND>(m_hwnd), WM_NULL, 0, 0);

    DestroyMenu(hMenu);
}

intptr_t HUB_CALLBACK TrayManager::static_wnd_proc(void* hwnd_ptr, uint32_t msg, uintptr_t wparam, intptr_t lparam) {
    auto hwnd = static_cast<HWND>(hwnd_ptr);

    if (msg == WM_CREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        return 0;
    }

    auto* self = reinterpret_cast<TrayManager*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) {
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    if (self->m_activation_msg_id != 0 && msg == self->m_activation_msg_id) {
        self->handle_command(CommandId::ShowHubWindow);
        return 0;
    }

    if (msg == WM_HUB_TRAYICON) {
        switch (LOWORD(lparam)) {
            case WM_LBUTTONUP:
            case WM_LBUTTONDBLCLK:
                self->handle_command(CommandId::ShowHubWindow);
                return 0;
            case WM_RBUTTONUP:
                self->show_context_menu();
                return 0;
            default:
                break;
        }
    } else if (msg == WM_COMMAND) {
        self->handle_command(static_cast<CommandId>(LOWORD(wparam)));
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}
#else
void TrayManager::show_context_menu() {}
intptr_t HUB_CALLBACK TrayManager::static_wnd_proc(void*, uint32_t, uintptr_t, intptr_t) { return 0; }
#endif

} // namespace hub::os
