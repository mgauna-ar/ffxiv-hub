#include "common/os/safe_memory.hpp"
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace hub::os {

bool safe_copy(void* dst, const void* src, size_t size) noexcept {
    if (dst == nullptr || src == nullptr) return false;
#ifdef _WIN32
    __try {
        std::memcpy(dst, src, size);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
#else
    std::memcpy(dst, src, size);
    return true;
#endif
}

} // namespace hub::os
