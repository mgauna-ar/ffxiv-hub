#pragma once

#include <cstdint>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace hub::os {

/**
 * @brief Move-only owner of an OS handle, closed when it goes out of scope.
 *
 * `Traits` supplies the handle type, the empty value, what counts as valid and
 * how to close one, so the ownership logic is the same on every platform and a
 * test can count closes without Win32.
 */
template <typename Traits>
class BasicUniqueHandle {
public:
    using pointer = typename Traits::pointer;

    BasicUniqueHandle() noexcept = default;
    explicit BasicUniqueHandle(pointer handle) noexcept : m_handle(handle) {}
    ~BasicUniqueHandle() { reset(); }

    BasicUniqueHandle(const BasicUniqueHandle&) = delete;
    BasicUniqueHandle& operator=(const BasicUniqueHandle&) = delete;

    BasicUniqueHandle(BasicUniqueHandle&& other) noexcept : m_handle(other.release()) {}
    BasicUniqueHandle& operator=(BasicUniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] pointer get() const noexcept { return m_handle; }
    [[nodiscard]] explicit operator bool() const noexcept { return Traits::is_valid(m_handle); }

    /// Gives up ownership without closing.
    [[nodiscard]] pointer release() noexcept {
        return std::exchange(m_handle, Traits::empty());
    }

    /// Closes the handle held, if any, and takes ownership of `handle`.
    void reset(pointer handle = Traits::empty()) noexcept {
        const pointer old = std::exchange(m_handle, handle);
        if (Traits::is_valid(old)) {
            Traits::close(old);
        }
    }

private:
    pointer m_handle{Traits::empty()};
};

/// A kernel object HANDLE, typed as `void*` so headers need no <windows.h>.
/// Both nullptr and INVALID_HANDLE_VALUE count as empty, since Win32 APIs fail
/// with one or the other.
struct Win32HandleTraits {
    using pointer = void*;
    static pointer empty() noexcept { return nullptr; }
    static bool is_valid(pointer handle) noexcept {
#ifdef _WIN32
        return handle != nullptr && handle != INVALID_HANDLE_VALUE;
#else
        return handle != nullptr && handle != reinterpret_cast<pointer>(static_cast<std::intptr_t>(-1));
#endif
    }
    static void close(pointer handle) noexcept {
#ifdef _WIN32
        CloseHandle(static_cast<HANDLE>(handle));
#else
        // The mock OS layer hands out no real handles, so there is nothing to close.
        (void)handle;
#endif
    }
};

using UniqueHandle = BasicUniqueHandle<Win32HandleTraits>;

} // namespace hub::os
