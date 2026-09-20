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

    [[nodiscard]] uintptr_t remote_module_handle() const { return m_remote_hmodule; }
    [[nodiscard]] const std::string& last_error() const { return m_last_error; }

private:
    std::string m_last_error;
    uintptr_t   m_remote_hmodule{0};
    [[maybe_unused]] void* m_target_process_handle{nullptr};
};

} // namespace hub::os
