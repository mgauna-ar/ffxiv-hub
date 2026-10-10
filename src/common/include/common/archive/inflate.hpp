#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace hub::archive {

/// Why inflate() stopped.
enum class InflateError : uint8_t {
    None,
    /// The stream ended before its final block did.
    Truncated,
    /// Block type 3, which RFC 1951 reserves.
    BadBlockType,
    /// A stored block whose length and its complement disagree.
    BadStoredLength,
    /// A dynamic block's code lengths do not make a usable Huffman code.
    BadCodeLengths,
    /// A code that decodes to no symbol, or to a length symbol RFC 1951 does not define.
    BadSymbol,
    /// A distance code past 29, or a match reaching back before the output.
    BadDistance,
    /// The output would pass `max_output`.
    TooLarge,
};

struct InflateResult {
    std::vector<uint8_t> data;
    InflateError error{InflateError::None};

    [[nodiscard]] bool ok() const noexcept { return error == InflateError::None; }
};

/// Decompresses a raw DEFLATE stream (RFC 1951, no zlib or gzip wrapper), up to and
/// including its final block. Fails rather than write more than `max_output` bytes,
/// and never reads past `input`, whatever the stream says.
[[nodiscard]] InflateResult inflate(std::span<const uint8_t> input, size_t max_output);

} // namespace hub::archive
