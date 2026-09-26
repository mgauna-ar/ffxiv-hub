#pragma once

#include <cstdint>

namespace hub::os {

/// Called by the payload, in game process `pid`, as it unloads. Leaves a named
/// kernel object that lives as long as that process, so an app started after the
/// unload can tell the resident but unloaded DLL from a live one: the unload
/// itself is otherwise remembered only by the app that sent it.
void mark_payload_unloaded(uint32_t pid);

/// Whether the payload in game process `pid` has called mark_payload_unloaded().
[[nodiscard]] bool payload_marked_unloaded(uint32_t pid);

} // namespace hub::os
