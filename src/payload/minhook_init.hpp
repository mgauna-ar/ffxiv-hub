#pragma once

// Windows-only: MinHook is not built for the mock payload.
#ifdef _WIN32
#include "MinHook.h"

namespace hub::payload {

/// Initializes MinHook once for the process. `HookManager` and `Dx11Hook` share one
/// MinHook, so whichever installs second finds it already initialized, which is
/// success rather than an error.
[[nodiscard]] inline bool initialize_minhook() noexcept {
    const MH_STATUS status = MH_Initialize();
    return status == MH_OK || status == MH_ERROR_ALREADY_INITIALIZED;
}

} // namespace hub::payload
#endif
