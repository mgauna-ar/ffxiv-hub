#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace hub::common {

/// SHA-256 (FIPS 180-4), fed in pieces.
class Sha256 {
public:
    using Digest = std::array<uint8_t, 32>;

    void update(std::span<const uint8_t> data) noexcept;

    /// The digest of everything fed so far. The hasher starts over afterwards.
    [[nodiscard]] Digest finish() noexcept;

    [[nodiscard]] static Digest hash(std::span<const uint8_t> data) noexcept;

private:
    void compress(const uint8_t* block) noexcept;

    std::array<uint32_t, 8> m_state{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::array<uint8_t, 64> m_block{};
    size_t m_block_len{0};
    uint64_t m_total_bytes{0};
};

/// Lowercase hex, two digits per byte.
[[nodiscard]] std::string to_hex(std::span<const uint8_t> bytes);

} // namespace hub::common
