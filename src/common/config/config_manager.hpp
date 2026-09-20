#pragma once

#include "hub/types.hpp"
#include "common/config/json.hpp"
#include <string>
#include <filesystem>
#include <mutex>

namespace hub::config {

class ConfigManager {
public:
    static ConfigManager& instance() noexcept {
        static ConfigManager s_instance;
        return s_instance;
    }

    /// Returns absolute path to config.json (%APPDATA%/ffxiv-hub/config.json)
    [[nodiscard]] std::filesystem::path get_config_path() const;

    /// Load configuration from disk into memory
    bool load();

    /// Save current configuration from memory to disk
    bool save();

    /// Access root JSON configuration document
    [[nodiscard]] JsonValue& root() noexcept { return m_root; }
    [[nodiscard]] const JsonValue& root() const noexcept { return m_root; }

    /// Clamps an overlay window rectangle to keep it visible inside screen boundaries
    static Rect clamp_geometry_to_screen(
        const Rect& rect,
        float screen_width = 1920.0f,
        float screen_height = 1080.0f
    ) noexcept;

    void set_custom_path_for_testing(std::filesystem::path path) {
        m_custom_path = std::move(path);
    }

private:
    ConfigManager();
    ~ConfigManager() = default;

    std::mutex m_mutex;
    JsonValue m_root{JsonValue::ObjectType{}};
    std::filesystem::path m_custom_path;
};

} // namespace hub::config
