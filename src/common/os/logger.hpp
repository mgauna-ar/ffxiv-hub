#pragma once

#include <string>
#include <string_view>
#include <cstdint>

namespace hub::os {

enum class LogLevel : uint8_t {
    Debug,
    Info,
    Warning,
    Error
};

/**
 * @brief Thread-safe diagnostic file logger with session rotation (hub.log / hub.prev.log).
 */
class Logger {
public:
    static bool init(const std::string& custom_path = "", bool rotate = true);
    static void log(LogLevel level, std::string_view message);

    static void debug(std::string_view message) { log(LogLevel::Debug, message); }
    static void info(std::string_view message)  { log(LogLevel::Info, message); }
    static void warn(std::string_view message)  { log(LogLevel::Warning, message); }
    static void error(std::string_view message) { log(LogLevel::Error, message); }

    [[nodiscard]] static std::string log_file_path();
    [[nodiscard]] static std::string previous_log_path(const std::string& current_path = "");
    [[nodiscard]] static std::string default_log_path();

    static void open_log_file();
    static void open_config_folder();
    static void shutdown();
};

} // namespace hub::os
