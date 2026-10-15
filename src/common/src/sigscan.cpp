#include "common/sigscan.hpp"
#include <cstring>
#include <cstdlib>

namespace hub::memory {

Signature Signature::parse(std::string_view pattern) {
    Signature sig;
    size_t i = 0;
    const size_t len = pattern.length();

    while (i < len) {
        // Skip whitespace
        while (i < len && (pattern[i] == ' ' || pattern[i] == '\t')) {
            ++i;
        }
        if (i >= len) break;

        if (pattern[i] == '?') {
            sig.bytes.push_back(0);
            sig.mask.push_back(false);
            ++i;
            if (i < len && pattern[i] == '?') {
                ++i;
            }
        } else if (i + 1 < len) {
            char hex[3] = { pattern[i], pattern[i + 1], '\0' };
            char* end = nullptr;
            const auto val = static_cast<uint8_t>(std::strtoul(hex, &end, 16));
            if (end != hex) {
                sig.bytes.push_back(val);
                sig.mask.push_back(true);
                i += 2;
            } else {
                ++i; // Skip invalid char
            }
        } else {
            ++i;
        }
    }

    return sig;
}

const uint8_t* find_pattern(
    const uint8_t* base,
    size_t size,
    const Signature& sig
) {
    if (!base || size == 0 || sig.empty() || sig.size() > size) {
        return nullptr;
    }

    const size_t sig_len = sig.size();
    const size_t scan_len = size - sig_len;

    const uint8_t first_byte = sig.bytes[0];
    const bool first_masked = sig.mask[0];

    for (size_t i = 0; i <= scan_len; ++i) {
        // Fast-check first byte
        if (first_masked && base[i] != first_byte) {
            continue;
        }

        bool match = true;
        for (size_t j = 1; j < sig_len; ++j) {
            if (sig.mask[j] && base[i + j] != sig.bytes[j]) {
                match = false;
                break;
            }
        }

        if (match) {
            return base + i;
        }
    }

    return nullptr;
}

const uint8_t* find_pattern(
    const uint8_t* base,
    size_t size,
    std::string_view pattern
) {
    const auto sig = Signature::parse(pattern);
    return find_pattern(base, size, sig);
}

uintptr_t resolve_rip_relative(
    uintptr_t instruction_addr,
    size_t disp_offset,
    size_t instruction_size
) {
    if (instruction_addr == 0) return 0;
    int32_t disp = 0;
    std::memcpy(&disp, reinterpret_cast<const void*>(instruction_addr + disp_offset), sizeof(int32_t));
    const uintptr_t rip = instruction_addr + instruction_size;
    return rip + static_cast<intptr_t>(disp);
}

namespace {
    // x86-64 CALL rel32 instruction layout: opcode (0xE8) + displacement (4 bytes int32)
    constexpr size_t CALL_REL32_DISP_OFFSET = 1;
    constexpr size_t CALL_REL32_INSN_SIZE = 5;
}

uintptr_t resolve_call_relative(uintptr_t call_addr) {
    if (call_addr == 0) return 0;
    return resolve_rip_relative(call_addr, CALL_REL32_DISP_OFFSET, CALL_REL32_INSN_SIZE);
}

} // namespace hub::memory
