#include "common/archive/inflate.hpp"

#include <array>
#include <utility>

namespace hub::archive {

namespace {

constexpr int kMaxBits = 15;
constexpr int kMaxLengthCodes = 286;
constexpr int kMaxDistanceCodes = 30;
constexpr int kFixedLengthCodes = 288;

constexpr std::array<uint16_t, 29> kLengthBase = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<uint8_t, 29> kLengthExtra = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<uint16_t, 30> kDistanceBase = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<uint8_t, 30> kDistanceExtra = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

/// Order the code length code lengths are sent in (RFC 1951 3.2.7).
constexpr std::array<uint8_t, 19> kCodeLengthOrder = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

/// Bits LSB first, as DEFLATE packs them. Past the end it reads zeros and sets
/// `overrun`, which every caller checks before trusting what it read.
class BitReader {
public:
    explicit BitReader(std::span<const uint8_t> input) : m_input(input) {}

    uint32_t bits(int count) {
        uint64_t value = m_buffer;
        while (m_count < count) {
            if (m_pos >= m_input.size()) {
                m_overrun = true;
                return 0;
            }
            value |= static_cast<uint64_t>(m_input[m_pos++]) << m_count;
            m_count += 8;
        }
        m_buffer = value >> count;
        m_count -= count;
        return static_cast<uint32_t>(value & ((uint64_t{1} << count) - 1));
    }

    /// Drops the rest of the current byte, for a stored block.
    void align() noexcept {
        m_buffer = 0;
        m_count = 0;
    }

    /// The next `length` whole bytes, or an empty span (and `overrun`) when fewer are left.
    std::span<const uint8_t> bytes(size_t length) {
        if (length > m_input.size() - m_pos) {
            m_overrun = true;
            return {};
        }
        const auto out = m_input.subspan(m_pos, length);
        m_pos += length;
        return out;
    }

    [[nodiscard]] bool overrun() const noexcept { return m_overrun; }

private:
    std::span<const uint8_t> m_input;
    size_t m_pos{0};
    uint64_t m_buffer{0};
    int m_count{0};
    bool m_overrun{false};
};

/// A canonical Huffman code: how many codes each length has, and the symbols in
/// code order.
struct Huffman {
    std::array<uint16_t, kMaxBits + 1> count{};
    std::array<uint16_t, kFixedLengthCodes> symbol{};
};

/// Builds `h` from `lengths`. Negative for an over-subscribed set, 0 for a complete
/// code, and positive (the codes left unused) for an incomplete one.
int build(Huffman& h, std::span<const uint8_t> lengths) {
    h.count.fill(0);
    for (const uint8_t length : lengths) {
        ++h.count[length];
    }
    if (static_cast<size_t>(h.count[0]) == lengths.size()) return 0;

    int left = 1;
    for (int len = 1; len <= kMaxBits; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return left;
    }

    std::array<uint16_t, kMaxBits + 1> offsets{};
    for (int len = 1; len < kMaxBits; ++len) {
        offsets[len + 1] = static_cast<uint16_t>(offsets[len] + h.count[len]);
    }
    for (size_t sym = 0; sym < lengths.size(); ++sym) {
        if (lengths[sym] != 0) {
            h.symbol[offsets[lengths[sym]]++] = static_cast<uint16_t>(sym);
        }
    }
    return left;
}

/// The next symbol, or -1 for a code `h` does not have.
int decode(BitReader& in, const Huffman& h) {
    int code = 0;
    int first = 0;
    int index = 0;
    for (int len = 1; len <= kMaxBits; ++len) {
        code |= static_cast<int>(in.bits(1));
        if (in.overrun()) return -1;
        const int count = h.count[len];
        if (code - count < first) {
            return h.symbol[static_cast<size_t>(index + (code - first))];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

class Inflater {
public:
    Inflater(std::span<const uint8_t> input, size_t max_output) : m_in(input), m_max(max_output) {}

    InflateResult run() {
        InflateResult result;
        bool last = false;
        while (!last && m_error == InflateError::None) {
            last = m_in.bits(1) != 0;
            const uint32_t type = m_in.bits(2);
            if (m_in.overrun()) {
                fail(InflateError::Truncated);
                break;
            }
            switch (type) {
                case 0: stored(); break;
                case 1: fixed(); break;
                case 2: dynamic(); break;
                default: fail(InflateError::BadBlockType); break;
            }
        }
        result.error = m_error;
        if (m_error == InflateError::None) result.data = std::move(m_out);
        return result;
    }

private:
    void fail(InflateError error) {
        if (m_error == InflateError::None) m_error = error;
    }

    /// Truncated when the reader ran out, `otherwise` when it did not.
    void fail_read(InflateError otherwise) {
        fail(m_in.overrun() ? InflateError::Truncated : otherwise);
    }

    void stored() {
        m_in.align();
        const auto header = m_in.bytes(4);
        if (m_in.overrun()) return fail(InflateError::Truncated);
        const uint32_t length = header[0] | (static_cast<uint32_t>(header[1]) << 8);
        const uint32_t complement = header[2] | (static_cast<uint32_t>(header[3]) << 8);
        if (length != (~complement & 0xFFFFu)) return fail(InflateError::BadStoredLength);
        const auto data = m_in.bytes(length);
        if (m_in.overrun()) return fail(InflateError::Truncated);
        if (length > m_max - m_out.size()) return fail(InflateError::TooLarge);
        m_out.insert(m_out.end(), data.begin(), data.end());
    }

    void fixed() {
        // Built once: the fixed code never changes.
        static const auto tables = [] {
            std::array<uint8_t, kFixedLengthCodes> lengths{};
            for (size_t sym = 0; sym < lengths.size(); ++sym) {
                lengths[sym] = static_cast<uint8_t>(sym < 144 ? 8 : sym < 256 ? 9 : sym < 280 ? 7 : 8);
            }
            std::pair<Huffman, Huffman> built;
            build(built.first, lengths);
            std::array<uint8_t, kMaxDistanceCodes> distances{};
            distances.fill(5);
            build(built.second, distances);
            return built;
        }();
        codes(tables.first, tables.second);
    }

    void dynamic() {
        const int nlen = static_cast<int>(m_in.bits(5)) + 257;
        const int ndist = static_cast<int>(m_in.bits(5)) + 1;
        const int ncode = static_cast<int>(m_in.bits(4)) + 4;
        if (m_in.overrun()) return fail(InflateError::Truncated);
        if (nlen > kMaxLengthCodes || ndist > kMaxDistanceCodes) return fail(InflateError::BadCodeLengths);

        std::array<uint8_t, kCodeLengthOrder.size()> code_lengths{};
        for (int i = 0; i < ncode; ++i) {
            code_lengths[kCodeLengthOrder[static_cast<size_t>(i)]] = static_cast<uint8_t>(m_in.bits(3));
        }
        if (m_in.overrun()) return fail(InflateError::Truncated);
        Huffman length_code;
        if (build(length_code, code_lengths) != 0) return fail(InflateError::BadCodeLengths);

        std::array<uint8_t, kMaxLengthCodes + kMaxDistanceCodes> lengths{};
        const int total = nlen + ndist;
        int index = 0;
        while (index < total) {
            const int sym = decode(m_in, length_code);
            if (sym < 0) return fail_read(InflateError::BadCodeLengths);
            if (sym < 16) {
                lengths[static_cast<size_t>(index++)] = static_cast<uint8_t>(sym);
                continue;
            }
            uint8_t repeat_length = 0;
            int repeat = 0;
            if (sym == 16) {
                if (index == 0) return fail(InflateError::BadCodeLengths);
                repeat_length = lengths[static_cast<size_t>(index - 1)];
                repeat = 3 + static_cast<int>(m_in.bits(2));
            } else if (sym == 17) {
                repeat = 3 + static_cast<int>(m_in.bits(3));
            } else {
                repeat = 11 + static_cast<int>(m_in.bits(7));
            }
            if (m_in.overrun()) return fail(InflateError::Truncated);
            if (index + repeat > total) return fail(InflateError::BadCodeLengths);
            while (repeat-- > 0) {
                lengths[static_cast<size_t>(index++)] = repeat_length;
            }
        }

        // Without an end-of-block code the block could never finish.
        if (lengths[256] == 0) return fail(InflateError::BadCodeLengths);

        // An incomplete code is only allowed when it is a single code.
        const std::span<const uint8_t> all(lengths.data(), static_cast<size_t>(total));
        Huffman lencode;
        const int len_left = build(lencode, all.first(static_cast<size_t>(nlen)));
        if (len_left < 0 || (len_left > 0 && nlen - lencode.count[0] != 1)) {
            return fail(InflateError::BadCodeLengths);
        }
        Huffman distcode;
        const int dist_left = build(distcode, all.subspan(static_cast<size_t>(nlen)));
        if (dist_left < 0 || (dist_left > 0 && ndist - distcode.count[0] != 1)) {
            return fail(InflateError::BadCodeLengths);
        }
        codes(lencode, distcode);
    }

    void codes(const Huffman& lencode, const Huffman& distcode) {
        for (;;) {
            int sym = decode(m_in, lencode);
            if (sym < 0) return fail_read(InflateError::BadSymbol);
            if (sym < 256) {
                if (m_out.size() >= m_max) return fail(InflateError::TooLarge);
                m_out.push_back(static_cast<uint8_t>(sym));
                continue;
            }
            if (sym == 256) return;

            sym -= 257;
            if (sym >= static_cast<int>(kLengthBase.size())) return fail(InflateError::BadSymbol);
            const size_t length = kLengthBase[static_cast<size_t>(sym)] +
                                  m_in.bits(kLengthExtra[static_cast<size_t>(sym)]);

            const int dsym = decode(m_in, distcode);
            if (dsym < 0) return fail_read(InflateError::BadDistance);
            if (dsym >= static_cast<int>(kDistanceBase.size())) return fail(InflateError::BadDistance);
            const size_t distance = kDistanceBase[static_cast<size_t>(dsym)] +
                                    m_in.bits(kDistanceExtra[static_cast<size_t>(dsym)]);
            if (m_in.overrun()) return fail(InflateError::Truncated);
            if (distance > m_out.size()) return fail(InflateError::BadDistance);
            if (length > m_max - m_out.size()) return fail(InflateError::TooLarge);

            // Byte by byte: a match may overlap the bytes it is producing.
            size_t from = m_out.size() - distance;
            for (size_t i = 0; i < length; ++i) {
                const uint8_t byte = m_out[from++];
                m_out.push_back(byte);
            }
        }
    }

    BitReader m_in;
    size_t m_max;
    std::vector<uint8_t> m_out;
    InflateError m_error{InflateError::None};
};

} // namespace

InflateResult inflate(std::span<const uint8_t> input, size_t max_output) {
    return Inflater(input, max_output).run();
}

} // namespace hub::archive
