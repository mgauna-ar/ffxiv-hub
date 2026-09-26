#pragma once

#include <string>

namespace hub::os {

/**
 * @brief Manages Windows logon auto-start registration via HKCU\Software\Microsoft\Windows\CurrentVersion\Run.
 */
class AutoStart {
public:
    static constexpr const wchar_t* REG_KEY_PATH = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    static constexpr const wchar_t* REG_VALUE_NAME = L"FFXIVHub";

    [[nodiscard]] static bool is_enabled();
    static bool set_enabled(bool enable, const std::string& custom_exe_path = "");
    [[nodiscard]] static std::string current_executable_path();
};

} // namespace hub::os
