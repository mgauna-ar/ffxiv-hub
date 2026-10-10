#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hub::archive {

/// CRC-32 as zip uses it (the reflected 0xEDB88320 polynomial). Pass the previous
/// result as `crc` to continue over more data.
[[nodiscard]] uint32_t crc32(std::span<const uint8_t> data, uint32_t crc = 0) noexcept;

/// One file as the central directory lists it.
struct ZipEntry {
    std::string name;
    uint16_t flags{0};
    uint16_t method{0};
    uint32_t crc32{0};
    uint32_t compressed_size{0};
    uint32_t uncompressed_size{0};
    uint32_t local_header_offset{0};
};

/**
 * @brief Reads files out of a zip held in memory.
 *
 * Sizes and CRCs come from the central directory, never the local headers, so an
 * entry written with a trailing data descriptor reads like any other. Handles
 * stored and deflated entries in a single-disk archive without Zip64.
 */
class ZipReader {
public:
    /// The archive's directory, or nullopt when `archive` is not a zip this reader
    /// handles. `archive` must outlive the reader.
    [[nodiscard]] static std::optional<ZipReader> open(std::span<const uint8_t> archive);

    [[nodiscard]] const std::vector<ZipEntry>& entries() const noexcept { return m_entries; }

    /// The entry named exactly `name`, or nullptr.
    [[nodiscard]] const ZipEntry* find(std::string_view name) const noexcept;

    /// The entry's contents, decompressed and checked against its size and CRC.
    /// nullopt, with the reason in `error`, for an encrypted entry, a method other
    /// than stored or deflate, one larger than `max_size`, or corrupt data.
    [[nodiscard]] std::optional<std::vector<uint8_t>> extract(const ZipEntry& entry, size_t max_size,
                                                              std::string* error = nullptr) const;

private:
    explicit ZipReader(std::span<const uint8_t> archive) : m_archive(archive) {}

    std::span<const uint8_t> m_archive;
    std::vector<ZipEntry> m_entries;
};

} // namespace hub::archive
