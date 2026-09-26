#pragma once

#include <ctime>

namespace hub::os {

/// Broken-down local time for a time_t: localtime_s on Windows, localtime_r on
/// the mock. Both are thread-safe, unlike std::localtime.
[[nodiscard]] std::tm local_time(std::time_t t) noexcept;

} // namespace hub::os
