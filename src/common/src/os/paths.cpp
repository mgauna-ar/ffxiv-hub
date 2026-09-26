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

void open_with_default_app(const std::filesystem::path& path) {
#ifdef _WIN32
    ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOW);
#else
#ifdef __APPLE__
    const char* opener = "open";
#else
    const char* opener = "xdg-open";
#endif
    // argv, not a command line: nothing in the path is interpreted by a shell.
    std::string target = path.native();
    std::string program = opener;
    char* argv[] = {program.data(), target.data(), nullptr};
    pid_t pid = 0;
    if (posix_spawnp(&pid, opener, nullptr, nullptr, argv, environ) != 0) return;
    // Reaped off the caller's thread; the opener may wait on the handler it starts.
    std::thread([pid] {
        int status = 0;
        waitpid(pid, &status, 0);
    }).detach();
#endif
}

} // namespace hub::os
