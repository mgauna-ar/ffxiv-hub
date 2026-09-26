#include "common/os/logger.hpp"
#include "common/config/config_manager.hpp"

#include <fstream>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <filesystem>
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace hub::os {

namespace {

std::mutex g_log_mutex;
std::ofstream g_log_file;
std::string g_active_path;

std::string get_timestamp_string() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const auto timer = system_clock::to_time_t(now);

    std::tm bt{};
#ifdef _WIN32
    localtime_s(&bt, &timer);
#else
    localtime_r(&timer, &bt);
#endif

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

bool Logger::init(const std::string& custom_path, bool rotate) {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_file.is_open()) {
        g_log_file.close();
    }

    g_active_path = custom_path.empty() ? default_log_path() : custom_path;

    try {
        std::filesystem::path fspath(g_active_path);
        if (fspath.has_parent_path()) {
            std::filesystem::create_directories(fspath.parent_path());
        }

        if (rotate && std::filesystem::exists(fspath)) {
            std::error_code ec;
            const auto file_size = std::filesystem::file_size(fspath, ec);
            if (!ec && file_size > 0) {
                const std::string prev = previous_log_path(g_active_path);
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

std::string Logger::log_file_path() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    return g_active_path.empty() ? default_log_path() : g_active_path;
}

std::string Logger::previous_log_path(const std::string& current_path) {
    std::string base = current_path;
    if (base.empty()) {
        base = log_file_path();
    }
    std::filesystem::path p(base);
    const std::string stem = p.stem().string();
    const std::string ext = p.extension().string();
    if (p.has_parent_path()) {
        return (p.parent_path() / (stem + ".prev" + ext)).string();
    }
    return stem + ".prev" + ext;
}

std::string Logger::default_log_path() {
    const auto cfg_path = hub::config::ConfigManager::instance().get_config_path();
    if (cfg_path.has_parent_path()) {
        return (cfg_path.parent_path() / "hub.log").string();
    }
    return "./hub.log";
}

void Logger::open_log_file() {
    const std::string path = log_file_path();
#ifdef _WIN32
    std::wstring wpath(path.begin(), path.end());
    ShellExecuteW(nullptr, L"open", wpath.c_str(), nullptr, nullptr, SW_SHOW);
#else
    std::string cmd = "open \"" + path + "\" 2>/dev/null || xdg-open \"" + path + "\" 2>/dev/null &";
    (void)std::system(cmd.c_str());
#endif
}

void Logger::open_config_folder() {
    const std::string path = log_file_path();
    std::filesystem::path fspath(path);
    std::string folder = fspath.has_parent_path() ? fspath.parent_path().string() : ".";
#ifdef _WIN32
    std::wstring wfolder(folder.begin(), folder.end());
    ShellExecuteW(nullptr, L"open", wfolder.c_str(), nullptr, nullptr, SW_SHOW);
#else
    std::string cmd = "open \"" + folder + "\" 2>/dev/null || xdg-open \"" + folder + "\" 2>/dev/null &";
    (void)std::system(cmd.c_str());
#endif
}

void Logger::shutdown() {
    std::lock_guard<std::mutex> lock(g_log_mutex);
    if (g_log_file.is_open()) {
        g_log_file << "FFXIV Hub Diagnostic Log Closed: " << get_timestamp_string() << "\n\n";
        g_log_file.close();
    }
}

} // namespace hub::os
