#include "common/os/auto_start.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <vector>
#else
#include <mutex>
#endif

namespace hub::os {

#ifndef _WIN32
namespace {
std::mutex g_mock_reg_mutex;
bool g_mock_auto_start_enabled = false;
std::string g_mock_exe_path = "/usr/local/bin/ffxiv-hub";
} // namespace
#endif

std::string AutoStart::current_executable_path() {
#ifdef _WIN32
    std::wstring path(MAX_PATH, L'\0');
    DWORD size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    while (size >= path.size()) {
        path.resize(path.size() * 2);
        size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    }
    path.resize(size);
    const int size_needed = WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), nullptr, 0, nullptr, nullptr);
    if (size_needed <= 0) return {};
    std::string result(size_needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, path.data(), static_cast<int>(path.size()), result.data(), size_needed, nullptr, nullptr);
    return result;
#else
    return g_mock_exe_path;
#endif
}

bool AutoStart::is_enabled() {
#ifdef _WIN32
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY_PATH, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    DWORD type = 0;
    DWORD data_size = 0;
    const LONG res = RegQueryValueExW(hKey, REG_VALUE_NAME, nullptr, &type, nullptr, &data_size);
    RegCloseKey(hKey);

    return (res == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ));
#else
    std::lock_guard<std::mutex> lock(g_mock_reg_mutex);
    return g_mock_auto_start_enabled;
#endif
}

bool AutoStart::set_enabled(bool enable, const std::string& custom_exe_path) {
#ifdef _WIN32
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_KEY_PATH, 0, KEY_WRITE, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    bool success = false;
    if (enable) {
        const std::string exe_path = custom_exe_path.empty() ? current_executable_path() : custom_exe_path;
        if (!exe_path.empty()) {
            const std::string quoted = "\"" + exe_path + "\"";
            const int wlen = MultiByteToWideChar(CP_UTF8, 0, quoted.c_str(), -1, nullptr, 0);
            if (wlen > 0) {
                std::vector<wchar_t> wstr(wlen);
                MultiByteToWideChar(CP_UTF8, 0, quoted.c_str(), -1, wstr.data(), wlen);
                const LONG set_res = RegSetValueExW(
                    hKey,
                    REG_VALUE_NAME,
                    0,
                    REG_SZ,
                    reinterpret_cast<const BYTE*>(wstr.data()),
                    static_cast<DWORD>(wstr.size() * sizeof(wchar_t))
                );
                success = (set_res == ERROR_SUCCESS);
            }
        }
    } else {
        const LONG del_res = RegDeleteValueW(hKey, REG_VALUE_NAME);
        success = (del_res == ERROR_SUCCESS || del_res == ERROR_FILE_NOT_FOUND);
    }

    RegCloseKey(hKey);
    return success;
#else
    (void)custom_exe_path;
    std::lock_guard<std::mutex> lock(g_mock_reg_mutex);
    g_mock_auto_start_enabled = enable;
    return true;
#endif
}

} // namespace hub::os
