#include "common/os/paths.hpp"

#include <cstdlib>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <thread>

extern char** environ;
#endif

namespace hub::os {

std::filesystem::path app_data_dir() {
#ifdef _WIN32
    // Sized by a first call, since a profile path is not bounded by MAX_PATH.
    const DWORD needed = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
    if (needed <= 1) return {};
    std::wstring appdata(needed, L'\0');
    const DWORD len = GetEnvironmentVariableW(L"APPDATA", appdata.data(), needed);
    if (len == 0 || len >= needed) return {};
    appdata.resize(len);
    return std::filesystem::path(appdata) / L"ffxiv-hub";
#else
    const char* home = std::getenv("HOME");
    if (home == nullptr || home[0] == '\0') return {};
    return std::filesystem::path(home) / ".config" / "ffxiv-hub";
#endif
}

std::filesystem::path executable_dir() {
#ifdef _WIN32
    wchar_t exe_path_buf[MAX_PATH];
    const DWORD len = GetModuleFileNameW(nullptr, exe_path_buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    return std::filesystem::path(exe_path_buf).parent_path();
#else
    return {};
#endif
}

std::string to_utf8(const std::filesystem::path& path) {
    const std::u8string utf8 = path.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

#ifndef _WIN32
namespace {

/// Runs `opener target` without a shell, reaped off the caller's thread since the
/// opener may wait on the handler it starts.
void spawn_opener(std::string target) {
#ifdef __APPLE__
    const char* opener = "open";
#else
    const char* opener = "xdg-open";
#endif
    std::string program = opener;
    char* argv[] = {program.data(), target.data(), nullptr};
    pid_t pid = 0;
    if (posix_spawnp(&pid, opener, nullptr, nullptr, argv, environ) != 0) return;
    std::thread([pid] {
        int status = 0;
        waitpid(pid, &status, 0);
    }).detach();
}

} // namespace
#endif

void open_with_default_app(const std::filesystem::path& path) {
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOW);
#else
    spawn_opener(path.native());
#endif
}

void open_url(std::string_view url) {
    if (!url.starts_with("https://")) return;
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", to_wide(url).c_str(), nullptr, nullptr, SW_SHOW);
#else
    spawn_opener(std::string(url));
#endif
}

#ifdef _WIN32
std::wstring to_wide(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
    return wide;
}
#endif

} // namespace hub::os
