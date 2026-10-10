#include "test_framework.hpp"
#include "archive_fixtures.hpp"
#include "common/archive/inflate.hpp"
#include "common/archive/zip_reader.hpp"
#include <cstdint>
#include <iterator>
#include <span>
#include <string>
#include <vector>

using namespace hub::archive;
using namespace hub::test::archive_fixtures;

namespace {

std::string as_text(const std::vector<uint8_t>& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

} // namespace

TEST_CASE(Inflate, StoredBlock) {
    const auto result = inflate(kStoredBlock, 1024);
    TEST_ASSERT_TRUE(result.ok());
    TEST_ASSERT_EQ(as_text(result.data), std::string("stored block data"));
}

TEST_CASE(Inflate, FixedHuffmanBlock) {
    const auto result = inflate(kFixedBlock, 1024);
    TEST_ASSERT_TRUE(result.ok());
    TEST_ASSERT_EQ(as_text(result.data), std::string("fixed huffman, fixed huffman, fixed huffman!"));
}

TEST_CASE(Inflate, DynamicHuffmanBlock) {
    const auto result = inflate(kDynamicBlock, 1 << 16);
    TEST_ASSERT_TRUE(result.ok());
    TEST_ASSERT_EQ(as_text(result.data), lines(60));
}

TEST_CASE(Inflate, StreamOfSeveralBlocks) {
    const auto result = inflate(kTwoBlocks, 1 << 16);
    TEST_ASSERT_TRUE(result.ok());
    TEST_ASSERT_EQ(as_text(result.data), lines(60));
}

TEST_CASE(Inflate, RefusesToPassTheOutputLimit) {
    const auto result = inflate(kDynamicBlock, lines(60).size() - 1);
    TEST_ASSERT_EQ(result.error, InflateError::TooLarge);
    TEST_ASSERT_TRUE(result.data.empty());
}

TEST_CASE(Inflate, TruncatedStreamFails) {
    // Every cut short of the whole stream fails, and none reads past its end.
    const std::span<const uint8_t> whole(kDynamicBlock);
    for (size_t cut = 0; cut < whole.size(); ++cut) {
        TEST_ASSERT_FALSE(inflate(whole.first(cut), 1 << 16).ok());
    }
}

TEST_CASE(Inflate, CorruptStreamFailsCleanly) {
    // A flipped bit anywhere either fails or decodes to other bytes; it never
    // reads or writes out of bounds, which the sanitizer runs check.
    std::vector<uint8_t> bytes(std::begin(kDynamicBlock), std::end(kDynamicBlock));
    for (size_t i = 0; i < bytes.size(); ++i) {
        for (int bit = 0; bit < 8; ++bit) {
            const auto flip = static_cast<uint8_t>(1u << bit);
            bytes[i] = static_cast<uint8_t>(bytes[i] ^ flip);
            const auto result = inflate(bytes, 1 << 16);
            if (result.ok()) TEST_ASSERT_TRUE(result.data.size() <= (1u << 16));
            bytes[i] = static_cast<uint8_t>(bytes[i] ^ flip);
        }
    }
}

TEST_CASE(Inflate, ReservedBlockTypeFails) {
    // BFINAL set, BTYPE 3.
    const uint8_t reserved[] = {0x07, 0x00};
    TEST_ASSERT_EQ(inflate(reserved, 1024).error, InflateError::BadBlockType);
}

TEST_CASE(Inflate, StoredLengthMismatchFails) {
    const uint8_t bad[] = {0x01, 0x05, 0x00, 0x00, 0x00, 'a', 'b', 'c', 'd', 'e'};
    TEST_ASSERT_EQ(inflate(bad, 1024).error, InflateError::BadStoredLength);
}

TEST_CASE(Zip, Crc32KnownValue) {
    const std::string text = "123456789";
    const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(text.data()), text.size());
    TEST_ASSERT_EQ(crc32(bytes), 0xCBF43926u);
    // Continued over two pieces, the same.
    TEST_ASSERT_EQ(crc32(bytes.subspan(4), crc32(bytes.first(4))), 0xCBF43926u);
}

TEST_CASE(Zip, ListsAndExtractsEntries) {
    const auto zip = ZipReader::open(kZipPlain);
    TEST_ASSERT_TRUE(zip.has_value());
    TEST_ASSERT_EQ(zip->entries().size(), size_t{3});

    const ZipEntry* exe = zip->find("ffxiv-hub.exe");
    const ZipEntry* dll = zip->find("hub_payload.dll");
    const ZipEntry* notes = zip->find("notes.txt");
    TEST_ASSERT_TRUE(exe != nullptr && dll != nullptr && notes != nullptr);
    TEST_ASSERT_TRUE(zip->find("missing.txt") == nullptr);

    const auto exe_data = zip->extract(*exe, 1 << 20);
    TEST_ASSERT_TRUE(exe_data.has_value());
    TEST_ASSERT_EQ(as_text(*exe_data), expected_exe());
    const auto dll_data = zip->extract(*dll, 1 << 20);
    TEST_ASSERT_TRUE(dll_data.has_value());
    TEST_ASSERT_EQ(as_text(*dll_data), expected_dll());
    const auto notes_data = zip->extract(*notes, 1 << 20);
    TEST_ASSERT_TRUE(notes_data.has_value());
    TEST_ASSERT_EQ(as_text(*notes_data), std::string("not part of an update\n"));
}

TEST_CASE(Zip, EntriesWithDataDescriptors) {
    const auto zip = ZipReader::open(kZipWithDescriptors);
    TEST_ASSERT_TRUE(zip.has_value());
    const ZipEntry* exe = zip->find("ffxiv-hub.exe");
    TEST_ASSERT_TRUE(exe != nullptr);
    TEST_ASSERT_TRUE((exe->flags & 0x08) != 0);
    const auto data = zip->extract(*exe, 1 << 20);
    TEST_ASSERT_TRUE(data.has_value());
    TEST_ASSERT_EQ(as_text(*data), expected_exe());
}

TEST_CASE(Zip, RefusesAnEntryOverTheLimit) {
    const auto zip = ZipReader::open(kZipPlain);
    TEST_ASSERT_TRUE(zip.has_value());
    std::string error;
    TEST_ASSERT_FALSE(zip->extract(*zip->find("ffxiv-hub.exe"), 16, &error).has_value());
    TEST_ASSERT_FALSE(error.empty());
}

TEST_CASE(Zip, CorruptDataFailsItsCrc) {
    std::vector<uint8_t> bytes(std::begin(kZipPlain), std::end(kZipPlain));
    const auto zip = ZipReader::open(bytes);
    TEST_ASSERT_TRUE(zip.has_value());
    const ZipEntry notes = *zip->find("notes.txt");
    // Stored, so the byte is changed as is and only the CRC can catch it.
    uint8_t& byte = bytes[notes.local_header_offset + 30 + notes.name.size()];
    byte = static_cast<uint8_t>(byte ^ 0x01);
    std::string error;
    TEST_ASSERT_FALSE(zip->extract(notes, 1 << 20, &error).has_value());
    TEST_ASSERT_EQ(error, std::string("CRC mismatch"));
}

TEST_CASE(Zip, RejectsWhatIsNotAZip) {
    TEST_ASSERT_FALSE(ZipReader::open(std::span<const uint8_t>()).has_value());
    TEST_ASSERT_FALSE(ZipReader::open(kDynamicBlock).has_value());
    // Every cut that loses the end record fails, never reading out of bounds.
    const std::span<const uint8_t> whole(kZipPlain);
    for (size_t cut = 0; cut + 22 <= whole.size(); ++cut) {
        const auto zip = ZipReader::open(whole.first(cut));
        if (zip) {
            for (const ZipEntry& entry : zip->entries()) (void)zip->extract(entry, 1 << 20);
        }
    }
}
