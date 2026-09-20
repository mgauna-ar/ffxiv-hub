#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <span>
#include <string_view>

namespace hub::memory {

/// Parsed IDA-style byte pattern signature
struct Signature {
    std::vector<uint8_t> bytes;
    std::vector<bool>    mask; // true = exact byte match, false = wildcard

    static Signature parse(std::string_view pattern);

    [[nodiscard]] size_t size() const noexcept { return bytes.size(); }
    [[nodiscard]] bool empty() const noexcept { return bytes.empty(); }
};

/// Scan memory range [base, base + size) for parsed signature
const uint8_t* find_pattern(
    const uint8_t* base,
    size_t size,
    const Signature& sig
);

/// Convenience overload parsing pattern on the fly
const uint8_t* find_pattern(
    const uint8_t* base,
    size_t size,
    std::string_view pattern
);

/// Convenience overload scanning a std::span
inline const uint8_t* find_pattern(
    std::span<const uint8_t> memory,
    std::string_view pattern
) {
    return find_pattern(memory.data(), memory.size(), pattern);
}

/// Resolves a RIP-relative instruction address (e.g. MOV/LEA rcx, [rip + disp32])
uintptr_t resolve_rip_relative(
    uintptr_t instruction_addr,
    size_t disp_offset = 3,
    size_t instruction_size = 7
);

/// Resolves an x86-64 CALL rel32 displacement (E8 [disp32]) to its absolute target
uintptr_t resolve_call_relative(uintptr_t call_addr);

} // namespace hub::memory
