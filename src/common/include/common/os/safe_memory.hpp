#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace hub::os {

/**
 * @brief Copies `size` bytes from memory the game owns, surviving a bad pointer.
 *
 * On Windows the copy runs inside `__try / __except`, so an access violation from a
 * stale or freed game object returns false instead of taking the game down. The mock
 * build is a plain `memcpy`. False as well when either pointer is null. After a
 * failed copy `dst` may be partly written.
 *
 * This is the one SEH-guarded read: a reader copies the struct out through it and
 * then works on its own copy, so nothing with a destructor ever sits inside `__try`
 * (MSVC C2712).
 */
[[nodiscard]] bool safe_copy(void* dst, const void* src, size_t size) noexcept;

/**
 * @brief Sets `bits` in the 32-bit word at `dst`, which the game owns and may be
 * changing from its own thread, as one atomic OR.
 *
 * A read, OR and write of our own would put back a bit the game cleared in between,
 * or drop one it set. On Windows the OR is an interlocked one inside `__try / __except`;
 * the mock build ORs through `memcpy`. False when `dst` is null or the write faults.
 */
[[nodiscard]] bool safe_or32(void* dst, uint32_t bits) noexcept;

/// safe_copy of one `T` from `src`, which the game owns.
template <typename T>
[[nodiscard]] bool safe_read(const void* src, T& out) noexcept {
    static_assert(std::is_trivially_copyable_v<T>, "safe_read copies raw bytes");
    return safe_copy(&out, src, sizeof(T));
}

/// safe_read from an address held as an integer; 0 fails.
template <typename T>
[[nodiscard]] bool safe_read(uintptr_t src, T& out) noexcept {
    return safe_read(reinterpret_cast<const void*>(src), out);
}

} // namespace hub::os
