#pragma once

namespace hub::os {

/// True once the OS has started tearing the process down. Past this point the
/// loader may already have unloaded other modules, so cleanup that touches COM,
/// another DLL, a thread, or disk is unsafe and the OS reclaims it anyway.
[[nodiscard]] bool is_process_exiting() noexcept;

} // namespace hub::os
