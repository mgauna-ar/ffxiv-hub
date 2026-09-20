#include "common/os/process_finder.hpp"
#include <thread>
#include <chrono>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>

namespace hub::os {

namespace {

struct WindowSearchContext {
    DWORD pid{0};
    HWND best_hwnd{nullptr};
    int best_area{0};
    bool found_ffxiv_title{false};
};

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* ctx = reinterpret_cast<WindowSearchContext*>(lParam);
    DWORD proc_id = 0;
    GetWindowThreadProcessId(hwnd, &proc_id);
    if (proc_id == ctx->pid && IsWindowVisible(hwnd)) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const int area = (rc.right - rc.left) * (rc.bottom - rc.top);

        wchar_t class_name[256];
        class_name[0] = L'\0';
        GetClassNameW(hwnd, class_name, sizeof(class_name) / sizeof(wchar_t));

        wchar_t title_w[256];
        title_w[0] = L'\0';
        GetWindowTextW(hwnd, title_w, sizeof(title_w) / sizeof(wchar_t));

        const bool is_ffxiv_class = (wcscmp(class_name, L"FFXIVGAME") == 0);
        const bool is_ffxiv_title = (wcsstr(title_w, L"FINAL FANTASY XIV") != nullptr);

        if (is_ffxiv_class || is_ffxiv_title) {
            if (!ctx->found_ffxiv_title || area > ctx->best_area) {
                ctx->best_hwnd = hwnd;
                ctx->best_area = area;
                ctx->found_ffxiv_title = true;
            }
        } else if (!ctx->found_ffxiv_title) {
            if (area > ctx->best_area) {
                ctx->best_hwnd = hwnd;
                ctx->best_area = area;
            }
        }
    }
    return TRUE;
}

} // namespace

bool ProcessFinder::enable_debug_privilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        return false;
    }

    auto enable_priv = [&](LPCWSTR priv_name) -> bool {
        LUID luid{};
        if (!LookupPrivilegeValueW(nullptr, priv_name, &luid)) {
            return false;
        }
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        const BOOL res = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), nullptr, nullptr);
        return res && (GetLastError() != ERROR_NOT_ALL_ASSIGNED);
    };

    const bool debug_ok = enable_priv(L"SeDebugPrivilege");
    enable_priv(L"SeSecurityPrivilege");
    CloseHandle(token);
    return debug_ok;
}

void* ProcessFinder::find_game_window(uint32_t pid) {
    WindowSearchContext ctx;
    ctx.pid = pid;
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&ctx));
    return ctx.best_hwnd;
}

std::optional<ProcessInfo> ProcessFinder::find_process(std::string_view process_name) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(PROCESSENTRY32W);

    std::wstring target_name_w(process_name.begin(), process_name.end());
    std::optional<ProcessInfo> fallback_proc;

    if (snapshot != INVALID_HANDLE_VALUE && Process32FirstW(snapshot, &entry)) {
        do {
            const bool match = (_wcsicmp(entry.szExeFile, target_name_w.c_str()) == 0) ||
                               (_wcsicmp(entry.szExeFile, L"ffxiv_dx11.exe") == 0) ||
                               (_wcsicmp(entry.szExeFile, L"ffxiv.exe") == 0);
            if (match) {
                HANDLE h_process = OpenProcess(
                    PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                    PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE,
                    FALSE,
                    entry.th32ProcessID
                );

                const DWORD err = h_process ? 0 : GetLastError();
                const bool is_64 = h_process ? is_process_64_bit(h_process) : false;
                void* hwnd = find_game_window(entry.th32ProcessID);

                ProcessInfo info{
                    .pid = entry.th32ProcessID,
                    .name = std::string(process_name),
                    .is_64_bit = is_64,
                    .handle = h_process,
                    .window_handle = hwnd,
                    .last_error = err
                };

                if (h_process != nullptr) {
                    CloseHandle(snapshot);
                    return info;
                }

                if (!fallback_proc.has_value()) {
                    fallback_proc = info;
                }
            }
        } while (Process32NextW(snapshot, &entry));
    }

    if (snapshot != INVALID_HANDLE_VALUE) {
        CloseHandle(snapshot);
    }

    // Fallback: If not found in snapshot or couldn't open process with valid handle,
    // search directly by window class "FFXIVGAME"
    if (!fallback_proc.has_value() || fallback_proc->handle == nullptr) {
        HWND game_hwnd = FindWindowW(L"FFXIVGAME", nullptr);
        if (game_hwnd != nullptr) {
            DWORD win_pid = 0;
            GetWindowThreadProcessId(game_hwnd, &win_pid);
            if (win_pid != 0) {
                HANDLE h_process = OpenProcess(
                    PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                    PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE,
                    FALSE,
                    win_pid
                );

                const DWORD err = h_process ? 0 : GetLastError();
                const bool is_64 = h_process ? is_process_64_bit(h_process) : false;

                ProcessInfo win_info{
                    .pid = win_pid,
                    .name = std::string(process_name),
                    .is_64_bit = is_64,
                    .handle = h_process,
                    .window_handle = game_hwnd,
                    .last_error = err
                };

                if (h_process != nullptr) {
                    return win_info;
                }
                if (!fallback_proc.has_value()) {
                    fallback_proc = win_info;
                }
            }
        }
    }

    return fallback_proc;
}

std::optional<ProcessInfo> ProcessFinder::find_process_by_pid(uint32_t pid) {
    const HANDLE handle = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
        PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE,
        FALSE,
        pid
    );

    ProcessInfo info;
    info.pid = pid;
    info.name = "PID " + std::to_string(pid);
    info.handle = handle;
    info.last_error = (handle == nullptr) ? GetLastError() : 0;
    info.window_handle = find_game_window(pid);

    if (handle != nullptr) {
        info.is_64_bit = is_process_64_bit(handle);
    }

    return info;
}

bool ProcessFinder::is_process_64_bit(void* process_handle) {
    if (!process_handle) return false;

    BOOL is_wow64 = FALSE;
    if (!IsWow64Process(static_cast<HANDLE>(process_handle), &is_wow64)) {
        return false;
    }

    SYSTEM_INFO sys_info{};
    GetNativeSystemInfo(&sys_info);
    const bool is_os_64 = (sys_info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64);

    return is_os_64 && !is_wow64;
}

std::optional<ProcessInfo> ProcessFinder::wait_for_process(
    std::string_view process_name,
    uint32_t timeout_seconds
) {
    if (timeout_seconds == 0) {
        return find_process(process_name);
    }

    const auto start = std::chrono::steady_clock::now();
    int denied_retries = 0;
    constexpr int MAX_DENIED_RETRIES = 10;

    while (true) {
        auto proc = find_process(process_name);
        if (proc.has_value()) {
            if (proc->handle != nullptr) {
                return proc;
            }
            if (++denied_retries >= MAX_DENIED_RETRIES) {
                return proc;
            }
        } else {
            denied_retries = 0;
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start
        ).count();
        if (elapsed >= timeout_seconds) {
            return std::nullopt;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

} // namespace hub::os

#else // !_WIN32

namespace hub::os {

bool ProcessFinder::enable_debug_privilege() { return false; }
void* ProcessFinder::find_game_window(uint32_t) { return nullptr; }
std::optional<ProcessInfo> ProcessFinder::find_process(std::string_view) { return std::nullopt; }
std::optional<ProcessInfo> ProcessFinder::find_process_by_pid(uint32_t) { return std::nullopt; }
bool ProcessFinder::is_process_64_bit(void*) { return false; }
std::optional<ProcessInfo> ProcessFinder::wait_for_process(std::string_view, uint32_t) { return std::nullopt; }

} // namespace hub::os

#endif
