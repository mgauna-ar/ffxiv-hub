#pragma once

#include "common/os/process_finder.hpp"
#include <string>
#include <filesystem>

namespace hub::os {

/**
 * @brief Injects the unified hub payload DLL into the running game process.
 */
class DllInjector {
public:
    DllInjector() = default;
    ~DllInjector() = default;

    bool inject(const ProcessInfo& proc, const std::filesystem::path& dll_path);
    [[nodiscard]] static bool is_payload_already_loaded(const ProcessInfo& proc);

    [[nodiscard]] const std::string& last_error() const { return m_last_error; }

private:
    std::string m_last_error;
};

} // namespace hub::os
