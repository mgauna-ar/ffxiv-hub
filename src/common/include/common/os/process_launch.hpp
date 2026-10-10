#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace hub::os {

[[nodiscard]] uint32_t current_process_id() noexcept;

/// Starts `exe` with `args`, in the executable's folder, and does not wait for it.
/// False when it could not be started, and always on the mock.
[[nodiscard]] bool launch_detached(const std::filesystem::path& exe, const std::vector<std::string>& args);

/// Waits up to `timeout` for process `pid` to end. True once it has, or when there
/// is no such process to wait on; false on timeout. The mock returns true at once.
bool wait_for_process_exit(uint32_t pid, std::chrono::milliseconds timeout);

} // namespace hub::os
