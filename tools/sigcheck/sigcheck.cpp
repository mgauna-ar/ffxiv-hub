// Scans a ffxiv_dx11.exe on disk for the payload's signatures and reports how many
// times each one matches. Links the same matcher the payload uses, so a pattern that
// passes here is a pattern the payload will resolve.
//
// Usage: sigcheck <ffxiv_dx11.exe> <name=pattern>...

#include "common/sigscan.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

// Enough of the PE layout to find one section's bytes. The repo's pe_scanner only
// works on a module the OS already loaded, which is not an option off Windows.
struct Section {
    const uint8_t* data;
    size_t size;
};

bool find_section(const std::vector<uint8_t>& image, const char* want, Section& out) {
    if (image.size() < 0x40 || image[0] != 'M' || image[1] != 'Z') {
        return false;
    }
    uint32_t pe = 0;
    std::memcpy(&pe, image.data() + 0x3C, 4);
    if (pe + 24 > image.size() || std::memcmp(image.data() + pe, "PE\0\0", 4) != 0) {
        return false;
    }

    uint16_t section_count = 0;
    uint16_t optional_size = 0;
    std::memcpy(&section_count, image.data() + pe + 6, 2);
    std::memcpy(&optional_size, image.data() + pe + 20, 2);

    const size_t table = pe + 24 + optional_size;
    for (uint16_t i = 0; i < section_count; ++i) {
        const uint8_t* entry = image.data() + table + i * 40;
        if (table + (i + 1) * 40 > image.size()) {
            return false;
        }
        char name[9] = {0};
        std::memcpy(name, entry, 8);
        if (std::strcmp(name, want) != 0) {
            continue;
        }
        uint32_t raw_size = 0;
        uint32_t raw_offset = 0;
        std::memcpy(&raw_size, entry + 16, 4);
        std::memcpy(&raw_offset, entry + 20, 4);
        if (raw_offset + raw_size > image.size()) {
            return false;
        }
        out = {image.data() + raw_offset, raw_size};
        return true;
    }
    return false;
}

// The payload takes the first match, so more than one is as broken as none.
size_t count_matches(Section section, const hub::memory::Signature& sig) {
    size_t found = 0;
    const uint8_t* cursor = section.data;
    size_t remaining = section.size;
    while (remaining >= sig.size()) {
        const uint8_t* hit = hub::memory::find_pattern(cursor, remaining, sig);
        if (hit == nullptr) {
            break;
        }
        ++found;
        remaining -= static_cast<size_t>(hit - cursor) + 1;
        cursor = hit + 1;
    }
    return found;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: sigcheck <ffxiv_dx11.exe> <name=pattern>...\n");
        return 2;
    }

    std::ifstream file(argv[1], std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    std::vector<uint8_t> image((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());

    Section text{};
    if (!find_section(image, ".text", text)) {
        std::fprintf(stderr, "no .text section in %s\n", argv[1]);
        return 2;
    }
    std::printf(".text: %zu bytes\n\n", text.size);

    int failures = 0;
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        const size_t split = arg.find('=');
        const std::string name = arg.substr(0, split);
        const std::string pattern = arg.substr(split + 1);

        const auto sig = hub::memory::Signature::parse(pattern);
        if (sig.empty()) {
            std::printf("  %-36s UNPARSEABLE\n", name.c_str());
            ++failures;
            continue;
        }
        const size_t hits = count_matches(text, sig);
        const char* verdict = hits == 1 ? "OK" : (hits == 0 ? "BROKEN" : "AMBIGUOUS");
        failures += (hits != 1);
        std::printf("  %-36s %-10s (%zu hit%s)\n", name.c_str(), verdict, hits,
                    hits == 1 ? "" : "s");
    }

    std::printf("\n%s\n", failures == 0 ? "all signatures resolve uniquely"
                                        : "SOME SIGNATURES NEED ATTENTION");
    return failures == 0 ? 0 : 1;
}
