#include "common/archive/zip_reader.hpp"
#include "common/archive/inflate.hpp"

#include <array>

namespace hub::archive {

namespace {

constexpr uint32_t kEndOfDirectorySig = 0x06054b50;
constexpr uint32_t kDirectoryEntrySig = 0x02014b50;
constexpr uint32_t kLocalHeaderSig = 0x04034b50;

constexpr size_t kEndOfDirectorySize = 22;
constexpr size_t kDirectoryEntrySize = 46;
constexpr size_t kLocalHeaderSize = 30;
constexpr size_t kMaxCommentSize = 0xFFFF;

constexpr uint16_t kFlagEncrypted = 0x0001;
constexpr uint16_t kMethodStored = 0;
constexpr uint16_t kMethodDeflate = 8;

constexpr std::array<uint32_t, 256> kCrcTable = [] {
    std::array<uint32_t, 256> table{};
    for (uint32_t n = 0; n < table.size(); ++n) {
        uint32_t c = n;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        table[n] = c;
    }
    return table;
}();

/// `length` bytes at `offset` lie inside a buffer of `size`, without overflowing.
bool fits(size_t offset, size_t length, size_t size) noexcept {
    return offset <= size && length <= size - offset;
}

uint16_t read16(std::span<const uint8_t> data, size_t at) noexcept {
    return static_cast<uint16_t>(data[at] | (data[at + 1] << 8));
}

uint32_t read32(std::span<const uint8_t> data, size_t at) noexcept {
    return static_cast<uint32_t>(data[at]) | (static_cast<uint32_t>(data[at + 1]) << 8) |
           (static_cast<uint32_t>(data[at + 2]) << 16) | (static_cast<uint32_t>(data[at + 3]) << 24);
}

/// Where the end of central directory record starts, searched back from the end
/// over the longest comment it may carry.
std::optional<size_t> find_end_of_directory(std::span<const uint8_t> archive) noexcept {
    if (archive.size() < kEndOfDirectorySize) return std::nullopt;
    const size_t last = archive.size() - kEndOfDirectorySize;
    const size_t first = last > kMaxCommentSize ? last - kMaxCommentSize : 0;
    for (size_t at = last + 1; at-- > first;) {
        if (read32(archive, at) == kEndOfDirectorySig &&
            at + kEndOfDirectorySize + read16(archive, at + 20) <= archive.size()) {
            return at;
        }
    }
    return std::nullopt;
}

void set_error(std::string* error, const char* message) {
    if (error != nullptr) *error = message;
}

} // namespace

uint32_t crc32(std::span<const uint8_t> data, uint32_t crc) noexcept {
    crc = ~crc;
    for (const uint8_t byte : data) {
        crc = kCrcTable[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

std::optional<ZipReader> ZipReader::open(std::span<const uint8_t> archive) {
    const auto eocd = find_end_of_directory(archive);
    if (!eocd) return std::nullopt;

    const uint16_t this_disk = read16(archive, *eocd + 4);
    const uint16_t directory_disk = read16(archive, *eocd + 6);
    const uint16_t entries_here = read16(archive, *eocd + 8);
    const uint16_t entry_count = read16(archive, *eocd + 10);
    const uint32_t directory_size = read32(archive, *eocd + 12);
    const uint32_t directory_offset = read32(archive, *eocd + 16);
    // A split archive, or a Zip64 one, whose real values sit in another record.
    if (this_disk != 0 || directory_disk != 0 || entries_here != entry_count ||
        entry_count == 0xFFFF || directory_offset == 0xFFFFFFFFu) {
        return std::nullopt;
    }
    if (!fits(directory_offset, directory_size, *eocd)) return std::nullopt;

    ZipReader reader(archive);
    reader.m_entries.reserve(entry_count);
    size_t at = directory_offset;
    const size_t end = static_cast<size_t>(directory_offset) + directory_size;
    for (uint16_t i = 0; i < entry_count; ++i) {
        if (!fits(at, kDirectoryEntrySize, end) || read32(archive, at) != kDirectoryEntrySig) {
            return std::nullopt;
        }
        const size_t name_length = read16(archive, at + 28);
        const size_t extra_length = read16(archive, at + 30);
        const size_t comment_length = read16(archive, at + 32);
        const size_t record_size = kDirectoryEntrySize + name_length + extra_length + comment_length;
        if (!fits(at, record_size, end)) return std::nullopt;

        ZipEntry entry;
        entry.flags = read16(archive, at + 8);
        entry.method = read16(archive, at + 10);
        entry.crc32 = read32(archive, at + 16);
        entry.compressed_size = read32(archive, at + 20);
        entry.uncompressed_size = read32(archive, at + 24);
        entry.local_header_offset = read32(archive, at + 42);
        const auto name = archive.subspan(at + kDirectoryEntrySize, name_length);
        entry.name.assign(name.begin(), name.end());
        reader.m_entries.push_back(std::move(entry));
        at += record_size;
    }
    return reader;
}

const ZipEntry* ZipReader::find(std::string_view name) const noexcept {
    for (const ZipEntry& entry : m_entries) {
        if (entry.name == name) return &entry;
    }
    return nullptr;
}

std::optional<std::vector<uint8_t>> ZipReader::extract(const ZipEntry& entry, size_t max_size,
                                                       std::string* error) const {
    if ((entry.flags & kFlagEncrypted) != 0) {
        set_error(error, "entry is encrypted");
        return std::nullopt;
    }
    if (entry.method != kMethodStored && entry.method != kMethodDeflate) {
        set_error(error, "unsupported compression method");
        return std::nullopt;
    }
    if (entry.uncompressed_size > max_size) {
        set_error(error, "entry is larger than allowed");
        return std::nullopt;
    }

    // The local header's own name and extra field lengths may differ from the
    // directory's, so the data's start is read from it.
    const size_t header = entry.local_header_offset;
    if (!fits(header, kLocalHeaderSize, m_archive.size()) || read32(m_archive, header) != kLocalHeaderSig) {
        set_error(error, "bad local header");
        return std::nullopt;
    }
    const size_t data_offset = header + kLocalHeaderSize + read16(m_archive, header + 26) +
                               read16(m_archive, header + 28);
    if (!fits(data_offset, entry.compressed_size, m_archive.size())) {
        set_error(error, "entry runs past the archive");
        return std::nullopt;
    }
    const auto packed = m_archive.subspan(data_offset, entry.compressed_size);

    std::vector<uint8_t> data;
    if (entry.method == kMethodStored) {
        data.assign(packed.begin(), packed.end());
    } else {
        InflateResult inflated = inflate(packed, entry.uncompressed_size);
        if (!inflated.ok()) {
            set_error(error, "corrupt compressed data");
            return std::nullopt;
        }
        data = std::move(inflated.data);
    }
    if (data.size() != entry.uncompressed_size) {
        set_error(error, "size does not match the directory");
        return std::nullopt;
    }
    if (crc32(data) != entry.crc32) {
        set_error(error, "CRC mismatch");
        return std::nullopt;
    }
    return data;
}

} // namespace hub::archive
