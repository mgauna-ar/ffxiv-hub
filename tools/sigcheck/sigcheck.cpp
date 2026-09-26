// Counts how many times each of the payload's signatures matches a range of bytes
// in a file, with the same matcher the payload uses, so a pattern that passes here
// is a pattern the payload will resolve. tools/check_signatures.py finds the range
// (ffxiv_dx11.exe's .text) with tools/xivbin/pe.py and reports the results.
//
// Usage: sigcheck <file> <offset> <size> <name=pattern>...
// Prints one line per signature: name, match count and the first match's offset
// into the range, tab-separated. An unparseable pattern reads -1 matches; no match
// reads offset -1.

#include "common/sigscan.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct Matches {
    size_t count{0};
    const uint8_t* first{nullptr};
};

// The payload takes the first match, so more than one is as broken as none.
Matches find_all(const std::vector<uint8_t>& bytes, const hub::memory::Signature& sig) {
    Matches found;
    const uint8_t* cursor = bytes.data();
    size_t remaining = bytes.size();
    while (remaining >= sig.size()) {
        const uint8_t* hit = hub::memory::find_pattern(cursor, remaining, sig);
        if (hit == nullptr) {
            break;
        }
        if (found.count++ == 0) {
            found.first = hit;
        }
        remaining -= static_cast<size_t>(hit - cursor) + 1;
        cursor = hit + 1;
    }
    return found;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: sigcheck <file> <offset> <size> <name=pattern>...\n");
        return 2;
    }

    const auto offset = std::strtoull(argv[2], nullptr, 0);
    const auto size = std::strtoull(argv[3], nullptr, 0);
    std::ifstream file(argv[1], std::ios::binary);
    if (!file) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    std::vector<uint8_t> bytes(size);
    file.seekg(static_cast<std::streamoff>(offset));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size))) {
        std::fprintf(stderr, "cannot read %llu bytes at %llu from %s\n",
                     static_cast<unsigned long long>(size), static_cast<unsigned long long>(offset), argv[1]);
        return 2;
    }

    for (int i = 4; i < argc; ++i) {
        const std::string arg = argv[i];
        const size_t split = arg.find('=');
        const std::string name = arg.substr(0, split);
        const std::string pattern = arg.substr(split + 1);

        const auto sig = hub::memory::Signature::parse(pattern);
        if (sig.empty()) {
            std::printf("%s\t-1\t-1\n", name.c_str());
            continue;
        }
        const Matches hits = find_all(bytes, sig);
        const long long first = hits.first ? static_cast<long long>(hits.first - bytes.data()) : -1;
        std::printf("%s\t%zu\t%lld\n", name.c_str(), hits.count, first);
    }
    return 0;
}
