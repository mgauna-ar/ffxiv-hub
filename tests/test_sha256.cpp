#include "test_framework.hpp"
#include "common/sha256.hpp"
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

using hub::common::Sha256;
using hub::common::to_hex;

namespace {

std::span<const uint8_t> bytes_of(std::string_view text) {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

std::string hash_hex(std::string_view text) {
    return to_hex(Sha256::hash(bytes_of(text)));
}

} // namespace

// The FIPS 180-4 examples.
TEST_CASE(Sha256, KnownVectors) {
    TEST_ASSERT_EQ(hash_hex(""),
                   std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    TEST_ASSERT_EQ(hash_hex("abc"),
                   std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    TEST_ASSERT_EQ(hash_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
                   std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

TEST_CASE(Sha256, MillionAsInPieces) {
    // Fed in uneven pieces, so blocks straddle update() calls.
    const std::string piece(997, 'a');
    Sha256 hasher;
    size_t fed = 0;
    while (fed + piece.size() <= 1'000'000) {
        hasher.update(bytes_of(piece));
        fed += piece.size();
    }
    hasher.update(bytes_of(std::string(1'000'000 - fed, 'a')));
    TEST_ASSERT_EQ(to_hex(hasher.finish()),
                   std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

TEST_CASE(Sha256, PaddingBoundaries) {
    // Either side of 56 bytes, past which the length no longer fits after the pad
    // byte, and of a whole block. Split in two, each matches the one-piece hash.
    for (const size_t length : {size_t{55}, size_t{56}, size_t{63}, size_t{64}, size_t{65}}) {
        const std::string text(length, 'x');
        Sha256 split;
        split.update(bytes_of(std::string_view(text).substr(0, 7)));
        split.update(bytes_of(std::string_view(text).substr(7)));
        TEST_ASSERT_EQ(to_hex(split.finish()), hash_hex(text));
    }
    TEST_ASSERT_EQ(hash_hex(std::string(56, 'a')),
                   std::string("b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"));
}

TEST_CASE(Sha256, FinishStartsOver) {
    Sha256 hasher;
    hasher.update(bytes_of("abc"));
    (void)hasher.finish();
    TEST_ASSERT_EQ(to_hex(hasher.finish()),
                   std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}
