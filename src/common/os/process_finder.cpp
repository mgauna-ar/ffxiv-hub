#include "common/os/process_finder.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>

namespace hub::os {

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

                ProcessInfo info{
                    .pid = entry.th32ProcessID,
                    .name = std::string(process_name),
                    .handle = h_process,
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

                ProcessInfo win_info{
                    .pid = win_pid,
                    .name = std::string(process_name),
                    .handle = h_process,
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

} // namespace hub::os

#else // !_WIN32

namespace hub::os {

bool ProcessFinder::enable_debug_privilege() { return false; }
std::optional<ProcessInfo> ProcessFinder::find_process(std::string_view) { return std::nullopt; }

} // namespace hub::os

#endif
