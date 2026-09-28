#pragma once

#include "app/app_state.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace hub::app::ui {

/// What the Settings view remembers between frames: the log's last lines and the
/// run key, each polled on a timer rather than read from disk or the registry on
/// every frame.
struct SettingsViewState {
    std::vector<std::string> log_lines;
    /// hub.log as it was when last read. A poll that finds both unchanged skips
    /// the read.
    std::uintmax_t log_size{0};
    std::filesystem::file_time_type log_write_time{};
    std::chrono::steady_clock::time_point last_log_poll{};

    bool auto_start{false};
    std::chrono::steady_clock::time_point last_auto_start_poll{};
};

/// The last `max_lines` non-empty lines of `path`, reading no more than its final
/// `max_bytes`. Empty when the file cannot be opened.
[[nodiscard]] std::vector<std::string> read_log_tail(const std::filesystem::path& path, size_t max_lines,
                                                     std::uintmax_t max_bytes = 64 * 1024);

/**
 * @brief Renders the Hub Settings view.
 * Handles Windows auto-start configuration, live log inspection,
 * and configuration file management.
 */
void render_view_settings(AppState& app_state, SettingsViewState& state);

} // namespace hub::app::ui
