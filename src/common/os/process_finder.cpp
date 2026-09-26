#include "common/os/process_finder.hpp"
#include "common/os/unique_handle.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>

namespace hub::os {

bool ProcessFinder::enable_debug_privilege() {
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &raw_token)) {
        return false;
    }
    const UniqueHandle token(raw_token);

    // SeDebugPrivilege alone: it lets an elevated app open an elevated game.
    // Nothing here reads or writes a SACL, so SeSecurityPrivilege is not taken.
    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &luid)) {
        return false;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const BOOL res = AdjustTokenPrivileges(token.get(), FALSE, &tp, sizeof(TOKEN_PRIVILEGES), nullptr, nullptr);
    return res && (GetLastError() != ERROR_NOT_ALL_ASSIGNED);
}

namespace {

/// What injecting needs: a remote thread, memory to write the DLL path into, and a
/// wait on the process.
constexpr DWORD kGameProcessAccess = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                     PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | SYNCHRONIZE;

/// Opens `pid` for injection. The info is returned either way: without a handle,
/// last_error says why OpenProcess refused.
ProcessInfo open_game_process(DWORD pid, std::string_view process_name) {
    HANDLE h_process = OpenProcess(kGameProcessAccess, FALSE, pid);
    return ProcessInfo{
        .pid = pid,
        .name = std::string(process_name),
        .handle = h_process,
        .last_error = h_process ? 0 : GetLastError()
    };
}

} // namespace

std::optional<ProcessInfo> ProcessFinder::find_process(std::string_view process_name) {
    const UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(PROCESSENTRY32W);

    std::wstring target_name_w(process_name.begin(), process_name.end());
    std::optional<ProcessInfo> fallback_proc;

    if (snapshot && Process32FirstW(snapshot.get(), &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, target_name_w.c_str()) == 0) {
                ProcessInfo info = open_game_process(entry.th32ProcessID, process_name);
                if (info.handle != nullptr) {
                    return info;
                }

                if (!fallback_proc.has_value()) {
                    fallback_proc = info;
                }
            }
        } while (Process32NextW(snapshot.get(), &entry));
    }

    // Fallback: If not found in snapshot or couldn't open process with valid handle,
    // search directly by window class "FFXIVGAME"
    if (!fallback_proc.has_value() || fallback_proc->handle == nullptr) {
        HWND game_hwnd = FindWindowW(L"FFXIVGAME", nullptr);
        if (game_hwnd != nullptr) {
            DWORD win_pid = 0;
            GetWindowThreadProcessId(game_hwnd, &win_pid);
            if (win_pid != 0) {
                ProcessInfo win_info = open_game_process(win_pid, process_name);
                if (win_info.handle != nullptr) {
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
