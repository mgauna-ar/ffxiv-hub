// Candidate cross references into [lo, hi) from a raw .text section.
//
//     ffxiv_hub_xrefs <exe> <text_rawptr> <text_size> <text_va> <lo> <hi>
//
// Prints "<site_va> <kind>" per byte position whose rel32 lands in range: a
// RIP-relative disp32 ending the instruction (rip), followed by an imm8 (rip8)
// or an imm32 (rip32), or an E8/E9 call/jmp operand (rel). Overlapping matches
// are expected; pe.py confirms each one against the disassembly. Done in C++
// because a Python loop over 35 MB of .text takes minutes.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 7) {
        std::fprintf(stderr, "usage: %s exe rawptr size va lo hi\n", argv[0]);
        return 2;
    }
    std::FILE* f = std::fopen(argv[1], "rb");
    if (!f) return 1;
    const uint64_t raw = std::strtoull(argv[2], nullptr, 0);
    const uint64_t size = std::strtoull(argv[3], nullptr, 0);
    const uint64_t va = std::strtoull(argv[4], nullptr, 0);
    const uint64_t lo = std::strtoull(argv[5], nullptr, 0);
    const uint64_t hi = std::strtoull(argv[6], nullptr, 0);

    std::vector<uint8_t> text(size);
    std::fseek(f, static_cast<long>(raw), SEEK_SET);
    if (std::fread(text.data(), 1, size, f) != size) return 1;
    std::fclose(f);

    const struct { int extra; const char* kind; } tails[] = {{0, "rip"}, {1, "rip8"}, {4, "rip32"}};
    for (uint64_t p = 0; p + 4 <= size; ++p) {
        int32_t disp;
        std::memcpy(&disp, &text[p], 4);
        for (const auto& t : tails) {
            const uint64_t target = va + p + 4 + t.extra + static_cast<int64_t>(disp);
            if (target >= lo && target < hi) {
                std::printf("%llx %s\n", static_cast<unsigned long long>(va + p), t.kind);
            }
        }
        if (p > 0 && (text[p - 1] == 0xE8 || text[p - 1] == 0xE9)) {
            const uint64_t target = va + p + 4 + static_cast<int64_t>(disp);
            if (target >= lo && target < hi) {
                std::printf("%llx rel\n", static_cast<unsigned long long>(va + p - 1));
            }
        }
    }
    return 0;
}
