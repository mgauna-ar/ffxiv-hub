#include "test_framework.hpp"
#include "common/sigscan.hpp"
#include <vector>

using namespace hub::memory;

TEST_CASE(SigScan, ParseExactBytes) {
    const auto sig = Signature::parse("48 89 5C 24 08");
    TEST_ASSERT_EQ(sig.size(), 5u);
    TEST_ASSERT_EQ(sig.bytes[0], 0x48);
    TEST_ASSERT_EQ(sig.bytes[1], 0x89);
    TEST_ASSERT_EQ(sig.bytes[2], 0x5C);
    TEST_ASSERT_EQ(sig.bytes[3], 0x24);
    TEST_ASSERT_EQ(sig.bytes[4], 0x08);
    for (bool m : sig.mask) {
        TEST_ASSERT_TRUE(m);
    }
}

TEST_CASE(SigScan, ParseWildcards) {
    const auto sig = Signature::parse("48 8D 0D ? ? ? ? F3");
    TEST_ASSERT_EQ(sig.size(), 8u);
    TEST_ASSERT_TRUE(sig.mask[0]); // 48
    TEST_ASSERT_TRUE(sig.mask[1]); // 8D
    TEST_ASSERT_TRUE(sig.mask[2]); // 0D
    TEST_ASSERT_FALSE(sig.mask[3]); // ?
    TEST_ASSERT_FALSE(sig.mask[4]); // ?
    TEST_ASSERT_FALSE(sig.mask[5]); // ?
    TEST_ASSERT_FALSE(sig.mask[6]); // ?
    TEST_ASSERT_TRUE(sig.mask[7]); // F3
}

TEST_CASE(SigScan, FindPatternMatch) {
    const std::vector<uint8_t> buffer = {
        0x90, 0x90, 0x48, 0x89, 0x5C, 0x24, 0x08, 0xC3
    };

    const uint8_t* match = find_pattern(buffer.data(), buffer.size(), "48 89 5C 24 08");
    TEST_ASSERT(match != nullptr);
    TEST_ASSERT_EQ(match - buffer.data(), 2);
}

TEST_CASE(SigScan, FindPatternWildcardMatch) {
    const std::vector<uint8_t> buffer = {
        0x00, 0x11, 0x48, 0x8D, 0x0D, 0xAA, 0xBB, 0xCC, 0xDD, 0xF3, 0x90
    };

    const uint8_t* match = find_pattern(buffer.data(), buffer.size(), "48 8D 0D ? ? ? ? F3");
    TEST_ASSERT(match != nullptr);
    TEST_ASSERT_EQ(match - buffer.data(), 2);
}

TEST_CASE(SigScan, FindPatternNotFound) {
    const std::vector<uint8_t> buffer = { 0x01, 0x02, 0x03, 0x04 };
    const uint8_t* match = find_pattern(buffer.data(), buffer.size(), "FF EE DD");
    TEST_ASSERT(match == nullptr);
}

TEST_CASE(SigScan, ResolveRipRelative) {
    // Instruction: LEA rcx, [rip + 0x10]
    // Address: 0x1000, Length: 7, Disp offset: 3 -> Disp = 0x10
    // RIP = 0x1000 + 7 = 0x1007
    // Target = 0x1007 + 0x10 = 0x1017
    uint8_t insn[7] = { 0x48, 0x8D, 0x0D, 0x10, 0x00, 0x00, 0x00 };
    const auto addr = reinterpret_cast<uintptr_t>(insn);
    const uintptr_t target = resolve_rip_relative(addr, 3, 7);

    TEST_ASSERT_EQ(target, addr + 7 + 0x10);
}

TEST_CASE(SigScan, ResolveCallRelative) {
    // Instruction: CALL rel32 (0xE8 [disp32])
    // Length: 5, Disp offset: 1 -> Disp = 0x50
    uint8_t call_insn[5] = { 0xE8, 0x50, 0x00, 0x00, 0x00 };
    const auto addr = reinterpret_cast<uintptr_t>(call_insn);
    const uintptr_t target = resolve_call_relative(addr);

    TEST_ASSERT_EQ(target, addr + 5 + 0x50);
}
