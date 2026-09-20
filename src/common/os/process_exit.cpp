#include "common/os/process_exit.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace hub::os {

namespace {

using FnRtlDllShutdownInProgress = BOOLEAN(NTAPI*)();

// Resolved once: this is polled per presented frame.
FnRtlDllShutdownInProgress resolve_shutdown_query() noexcept {
    HMODULE h_ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!h_ntdll) return nullptr;
    return reinterpret_cast<FnRtlDllShutdownInProgress>(
        GetProcAddress(h_ntdll, "RtlDllShutdownInProgress")
    );
}

} // namespace

bool is_process_exiting() noexcept {
    static const FnRtlDllShutdownInProgress s_query = resolve_shutdown_query();
    return s_query != nullptr && s_query() != FALSE;
}

} // namespace hub::os

#else

namespace hub::os {

bool is_process_exiting() noexcept {
    return false;
}

} // namespace hub::os

#endif
