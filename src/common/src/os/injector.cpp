#include "common/os/injector.hpp"
#include "common/os/paths.hpp"
#include "common/os/logger.hpp"
#include "common/os/unique_handle.hpp"
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

    /// Memory committed in another process, released with VirtualFreeEx when the
    /// guard goes out of scope unless leak() was called.
    class RemoteAllocation {
    public:
        RemoteAllocation(HANDLE process, LPVOID address) noexcept
            : m_process(process), m_address(address) {}
        ~RemoteAllocation() {
            if (m_address) {
                VirtualFreeEx(m_process, m_address, 0, MEM_RELEASE);
            }
        }

        RemoteAllocation(const RemoteAllocation&) = delete;
        RemoteAllocation& operator=(const RemoteAllocation&) = delete;

        [[nodiscard]] LPVOID get() const noexcept { return m_address; }
        [[nodiscard]] explicit operator bool() const noexcept { return m_address != nullptr; }

        /// Keeps the memory allocated for the life of the target process.
        void leak() noexcept { m_address = nullptr; }

    private:
        HANDLE m_process;
        LPVOID m_address;
    };
} // namespace

bool DllInjector::inject(const ProcessInfo& proc, const std::filesystem::path& dll_path) {
    if (!proc.handle) {
        m_last_error = "Invalid process handle.";
        return false;
    }

    std::error_code ec;
    const auto abs_path = std::filesystem::absolute(dll_path, ec);
    if (ec || !std::filesystem::exists(abs_path, ec)) {
        m_last_error = "Payload DLL does not exist: " + to_utf8(dll_path);
        return false;
    }

    const std::wstring full_path_w = abs_path.wstring();
    const auto h_process = static_cast<HANDLE>(proc.handle);

    const size_t path_size_bytes = (full_path_w.length() + 1) * sizeof(wchar_t);

    RemoteAllocation remote_path(h_process, VirtualAllocEx(
        h_process,
        nullptr,
        path_size_bytes,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE
    ));
    if (!remote_path) {
        m_last_error = "VirtualAllocEx failed (Win32 Error: " + std::to_string(GetLastError()) + ")";
        return false;
    }

    SIZE_T bytes_written = 0;
    if (!WriteProcessMemory(h_process, remote_path.get(), full_path_w.c_str(), path_size_bytes, &bytes_written) ||
        bytes_written != path_size_bytes) {
        m_last_error = "WriteProcessMemory failed (Win32 Error: " + std::to_string(GetLastError()) + ")";
        return false;
    }

    HMODULE h_kernel32 = GetModuleHandleW(L"kernel32.dll");
    if (!h_kernel32) {
        m_last_error = "GetModuleHandleW(kernel32.dll) failed (Win32 Error: " + std::to_string(GetLastError()) + ")";
        return false;
    }

    auto pfn_load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(h_kernel32, "LoadLibraryW")
    );
    if (!pfn_load_library) {
        m_last_error = "GetProcAddress(LoadLibraryW) failed (Win32 Error: " + std::to_string(GetLastError()) + ")";
        return false;
    }

    DWORD thread_id = 0;
    const UniqueHandle remote_thread(CreateRemoteThread(
        h_process,
        nullptr,
        0,
        pfn_load_library,
        remote_path.get(),
        0,
        &thread_id
    ));
    if (!remote_thread) {
        m_last_error = "CreateRemoteThread failed (Win32 Error: " + std::to_string(GetLastError()) + ").";
        return false;
    }

    const DWORD wait_res = WaitForSingleObject(remote_thread.get(), INJECTION_THREAD_TIMEOUT_MS);
    if (wait_res != WAIT_OBJECT_0) {
        // LoadLibraryW may still be reading the path. Freeing it under the remote
        // thread would crash the game, so the few hundred bytes stay allocated.
        remote_path.leak();
        m_last_error = "Remote thread execution timed out or failed (wait result: " + std::to_string(wait_res) + ")";
        Logger::warn("Injection thread did not finish within " + std::to_string(INJECTION_THREAD_TIMEOUT_MS) +
                     " ms; leaving its " + std::to_string(path_size_bytes) +
                     "-byte path buffer allocated in the game process.");
        return false;
    }

    DWORD remote_exit_code = 0;
    if (!GetExitCodeThread(remote_thread.get(), &remote_exit_code)) {
        m_last_error = "GetExitCodeThread failed (Win32 Error: " + std::to_string(GetLastError()) + ")";
        return false;
    }

    if (remote_exit_code == 0 || remote_exit_code == STILL_ACTIVE) {
        m_last_error = "LoadLibraryW failed in game process (remote exit code: 0). Target path: " +
                       wstring_to_utf8(full_path_w);
        return false;
    }

    m_last_error = "OK";
    return true;
}

bool DllInjector::is_payload_already_loaded(const ProcessInfo& proc) {
    const UniqueHandle snap(CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, proc.pid));
    if (!snap) {
        return false;
    }

    MODULEENTRY32W me{};
    me.dwSize = sizeof(MODULEENTRY32W);
    bool found = false;

    if (Module32FirstW(snap.get(), &me)) {
        do {
            std::wstring mod = me.szModule;
            for (auto& c : mod) {
                c = static_cast<wchar_t>(towlower(c));
            }
            if (mod.find(L"hub_payload") != std::wstring::npos) {
                found = true;
                break;
            }
        } while (Module32NextW(snap.get(), &me));
    }

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
