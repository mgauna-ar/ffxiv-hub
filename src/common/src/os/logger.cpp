#include "common/os/logger.hpp"
#include "common/config/config_manager.hpp"
#include "common/os/local_time.hpp"
#include "common/os/paths.hpp"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <filesystem>
#include <mutex>

namespace hub::os {

namespace {

std::mutex g_log_mutex;
std::ofstream g_log_file;
std::filesystem::path g_active_path;

std::string get_timestamp_string() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const auto timer = system_clock::to_time_t(now);

    const std::tm bt = local_time(timer);

    std::ostringstream ss;
    ss << std::put_time(&bt, "%Y-%m-%d %H:%M:%S")
       << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

const char* level_to_string(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Debug:   return "DEBUG";
        case LogLevel::Info:    return "INFO ";
        case LogLevel::Warning: return "WARN ";
        case LogLevel::Error:   return "ERROR";
        default:                return "INFO ";
    }
}

} // namespace

bool Logger::init(const std::filesystem::path& custom_path, bool rotate) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_file.is_open()) {
        g_log_file.close();
    }

    g_active_path = custom_path.empty() ? default_log_path() : custom_path;

    try {
        const std::filesystem::path& fspath = g_active_path;
        if (fspath.has_parent_path()) {
            std::filesystem::create_directories(fspath.parent_path());
        }

        if (rotate && std::filesystem::exists(fspath)) {
            std::error_code ec;
            const auto file_size = std::filesystem::file_size(fspath, ec);
            if (!ec && file_size > 0) {
                const std::filesystem::path prev = previous_log_path(g_active_path);
                std::filesystem::remove(prev, ec);
                ec.clear();
                std::filesystem::rename(fspath, prev, ec);
            }
        }

        g_log_file.open(g_active_path, std::ios::out | std::ios::trunc);
        if (g_log_file.is_open()) {
            g_log_file << "=======================================================\n"
                       << "  FFXIV Hub Diagnostic Log Started: " << get_timestamp_string() << "\n"
                       << "=======================================================\n"
                       << std::flush;
            return true;
        }
    } catch (...) {
        // Fallback
    }

    return false;
}

void Logger::log(LogLevel level, std::string_view message) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    const std::string ts = get_timestamp_string();
    const char* lvl_str = level_to_string(level);

    if (g_log_file.is_open()) {
        g_log_file << "[" << ts << "] [" << lvl_str << "] " << message << "\n";
        g_log_file.flush();
    }
}

std::filesystem::path Logger::log_file_path() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    return g_active_path.empty() ? default_log_path() : g_active_path;
}

std::filesystem::path Logger::previous_log_path(const std::filesystem::path& current_path) {
    const std::filesystem::path base = current_path.empty() ? log_file_path() : current_path;
    std::filesystem::path prev = base;
    prev.replace_filename(base.stem());
    prev += ".prev";
    prev += base.extension();
    return prev;
}

std::filesystem::path Logger::default_log_path() {
    const auto cfg_path = hub::config::ConfigManager::instance().get_config_path();
    if (cfg_path.has_parent_path()) {
        return cfg_path.parent_path() / "hub.log";
    }
    return "hub.log";
}

void Logger::open_log_file() {
    open_with_default_app(log_file_path());
}

void Logger::open_log_folder() {
    const std::filesystem::path log_path = log_file_path();
    open_with_default_app(log_path.has_parent_path() ? log_path.parent_path() : std::filesystem::path("."));
}

void Logger::open_config_folder() {
    const auto cfg_path = hub::config::ConfigManager::instance().get_config_path();
    open_with_default_app(cfg_path.has_parent_path() ? cfg_path.parent_path() : std::filesystem::path("."));
}

void Logger::shutdown() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_file.is_open()) {
        g_log_file << "FFXIV Hub Diagnostic Log Closed: " << get_timestamp_string() << "\n\n";
        g_log_file.close();
    }
}

} // namespace hub::os
