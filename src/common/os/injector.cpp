#include "common/os/injector.hpp"
#include <filesystem>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>

namespace hub::os {

namespace {
    constexpr DWORD INJECTION_THREAD_TIMEOUT_MS = 10000;

    std::string wstring_to_utf8(const std::wstring& wstr) {
        if (wstr.empty()) return {};
        const int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
        if (size_needed <= 0) return {};
        std::string str(static_cast<size_t>(size_needed), '\0');
        WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), str.data(), size_needed, nullptr, nullptr);
        return str;
    }
} // namespace

bool DllInjector::inject(const ProcessInfo& proc, const std::filesystem::path& dll_path) {
    if (!proc.handle) {
        m_last_error = "Invalid process handle.";
        return false;
    }

    std::error_code ec;
    const auto abs_path = std::filesystem::absolute(dll_path, ec);
    if (ec || !std::filesystem::exists(abs_path, ec)) {
        m_last_error = "Payload DLL does not exist: " + dll_path.string();
        return false;
    }

    const std::wstring full_path_w = abs_path.wstring();
    m_target_process_handle = proc.handle;
    const auto h_process = static_cast<HANDLE>(proc.handle);

    const size_t path_size_bytes = (full_path_w.length() + 1) * sizeof(wchar_t);

    LPVOID p_remote_path = VirtualAllocEx(
        h_process,
        nullptr,
        path_size_bytes,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE
    );
    if (!p_remote_path) {
        m_last_error = "VirtualAllocEx failed (Win32 Error: " + std::to_string(GetLastError()) + ")";
        return false;
    }

    SIZE_T bytes_written = 0;
    if (!WriteProcessMemory(h_process, p_remote_path, full_path_w.c_str(), path_size_bytes, &bytes_written) ||
        bytes_written != path_size_bytes) {
        m_last_error = "WriteProcessMemory failed (Win32 Error: " + std::to_string(GetLastError()) + ")";
        VirtualFreeEx(h_process, p_remote_path, 0, MEM_RELEASE);
        return false;
    }

    HMODULE h_kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!h_kernel32) {
        m_last_error = "GetModuleHandleW(kernel32.dll) failed (Win32 Error: " + std::to_string(GetLastError()) + ")";
        VirtualFreeEx(h_process, p_remote_path, 0, MEM_RELEASE);
        return false;
    }

    auto pfn_load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(h_kernel32, "LoadLibraryW")
    );
    if (!pfn_load_library) {
        m_last_error = "GetProcAddress(LoadLibraryW) failed (Win32 Error: " + std::to_string(GetLastError()) + ")";
        VirtualFreeEx(h_process, p_remote_path, 0, MEM_RELEASE);
        return false;
    }

    DWORD thread_id = 0;
    HANDLE h_remote_thread = CreateRemoteThread(
        h_process,
        nullptr,
        0,
        pfn_load_library,
        p_remote_path,
        0,
        &thread_id
    );

    if (!h_remote_thread) {
        m_last_error = "CreateRemoteThread failed (Win32 Error: " + std::to_string(GetLastError()) + ").";
        VirtualFreeEx(h_process, p_remote_path, 0, MEM_RELEASE);
        return false;
    }

    const DWORD wait_res = WaitForSingleObject(h_remote_thread, INJECTION_THREAD_TIMEOUT_MS);
    if (wait_res != WAIT_OBJECT_0) {
        m_last_error = "Remote thread execution timed out or failed (wait result: " + std::to_string(wait_res) + ")";
        CloseHandle(h_remote_thread);
        VirtualFreeEx(h_process, p_remote_path, 0, MEM_RELEASE);
        return false;
    }

    DWORD remote_exit_code = 0;
    if (!GetExitCodeThread(h_remote_thread, &remote_exit_code)) {
        m_last_error = "GetExitCodeThread failed (Win32 Error: " + std::to_string(GetLastError()) + ")";
        CloseHandle(h_remote_thread);
        VirtualFreeEx(h_process, p_remote_path, 0, MEM_RELEASE);
        return false;
    }

    if (remote_exit_code == 0 || remote_exit_code == STILL_ACTIVE) {
        m_last_error = "LoadLibraryW failed in game process (remote exit code: 0). Target path: " +
                       wstring_to_utf8(full_path_w);
        CloseHandle(h_remote_thread);
        VirtualFreeEx(h_process, p_remote_path, 0, MEM_RELEASE);
        return false;
    }

    CloseHandle(h_remote_thread);
    VirtualFreeEx(h_process, p_remote_path, 0, MEM_RELEASE);

    m_remote_hmodule = static_cast<uintptr_t>(remote_exit_code);
    m_last_error = "OK";
    return true;
}

bool DllInjector::is_payload_already_loaded(const ProcessInfo& proc) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, proc.pid);
    if (snap == INVALID_HANDLE_VALUE) {
        return false;
    }

    MODULEENTRY32W me{};
    me.dwSize = sizeof(MODULEENTRY32W);
    bool found = false;

    if (Module32FirstW(snap, &me)) {
        do {
            std::wstring mod = me.szModule;
            for (auto& c : mod) {
                c = static_cast<wchar_t>(towlower(c));
            }
            if (mod.find(L"hub_payload") != std::wstring::npos) {
                found = true;
                break;
            }
        } while (Module32NextW(snap, &me));
    }

    CloseHandle(snap);
    return found;
}

} // namespace hub::os

#else // !_WIN32

namespace hub::os {

bool DllInjector::inject(const ProcessInfo&, const std::filesystem::path&) {
    m_last_error = "Injection not supported on non-Windows host.";
    return false;
}

bool DllInjector::is_payload_already_loaded(const ProcessInfo&) {
    return false;
}

} // namespace hub::os

#endif
