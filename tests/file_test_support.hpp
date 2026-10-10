#pragma once

// Files on disk for the tests that need them: test_os.cpp and test_updater.cpp.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

namespace hub::test::file_support {

/// A folder of its own under the system temp folder, removed with everything in
/// it when the test ends.
struct ScratchDir {
    std::filesystem::path path;
    explicit ScratchDir(const char* name)
        : path(std::filesystem::temp_directory_path() / name) {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
        std::filesystem::create_directories(path, ec);
    }
    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
};

inline std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

inline void write_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

} // namespace hub::test::file_support
