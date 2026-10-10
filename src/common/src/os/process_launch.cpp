#include "common/os/process_launch.hpp"

#ifdef _WIN32
#include "common/os/paths.hpp"
#include "common/os/unique_handle.hpp"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace hub::os {

#ifdef _WIN32
namespace {

/// `arg` quoted the way CommandLineToArgvW splits it back.
std::wstring quote_argument(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring quoted = L"\"";
    size_t backslashes = 0;
    for (const wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
            continue;
        }
        // Backslashes are literal unless a quote follows them.
        quoted.append(c == L'"' ? backslashes * 2 + 1 : backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(c);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

} // namespace

uint32_t current_process_id() noexcept {
    return GetCurrentProcessId();
}

bool launch_detached(const std::filesystem::path& exe, const std::vector<std::string>& args) {
    std::wstring command_line = quote_argument(exe.native());
    for (const std::string& arg : args) {
        command_line += L' ';
        command_line += quote_argument(to_wide(arg));
    }
    const std::wstring working_dir = exe.parent_path().native();

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(exe.c_str(), command_line.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        working_dir.empty() ? nullptr : working_dir.c_str(), &startup, &info)) {
        return false;
    }
    const UniqueHandle process(info.hProcess);
    const UniqueHandle thread(info.hThread);
    return true;
}

bool wait_for_process_exit(uint32_t pid, std::chrono::milliseconds timeout) {
    const UniqueHandle process(OpenProcess(SYNCHRONIZE, FALSE, pid));
    if (!process) return true;
    return WaitForSingleObject(process.get(), static_cast<DWORD>(timeout.count())) == WAIT_OBJECT_0;
}

#else

uint32_t current_process_id() noexcept {
    return static_cast<uint32_t>(getpid());
}

bool launch_detached(const std::filesystem::path&, const std::vector<std::string>&) {
    return false;
}

bool wait_for_process_exit(uint32_t, std::chrono::milliseconds) {
    return true;
}

#endif

} // namespace hub::os
