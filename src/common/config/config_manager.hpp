#pragma once

#include "hub/types.hpp"
#include "common/config/json.hpp"
#include <string>
#include <filesystem>
#include <mutex>
#include <optional>
#include <vector>

namespace hub::config {

class ConfigManager {
public:
    /// The process-wide document. Separate instances exist only so tests can
    /// stand in for the app and the payload writing one file.
    static ConfigManager& instance() noexcept {
        static ConfigManager s_instance;
        return s_instance;
    }

    ConfigManager();
    ~ConfigManager() = default;

    /// Returns absolute path to config.json (%APPDATA%/ffxiv-hub/config.json)
    [[nodiscard]] std::filesystem::path get_config_path() const;

    /// Load configuration from disk into memory
    bool load();

    /// Save current configuration from memory to disk. With owned sections set,
    /// only those sections' keys are merged into the file as it is now on disk.
    bool save();

    /// Replaces the in-memory document with the defaults. Does not touch disk.
    void reset_to_defaults();

    /// Every key a plugin reads, at its default. Master switches are left out so
    /// the combat meter's legacy "enabled" key keeps its meaning on load.
    [[nodiscard]] static JsonValue default_document();

    /// Sections this process writes back. Empty (the default) writes the whole
    /// document. The payload scopes itself to its plugin sections so its stale
    /// copy of app-only keys never overwrites what the app saved since.
    void set_owned_sections(std::vector<std::string> sections);

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
    static std::optional<JsonValue> read_disk_document(const std::filesystem::path& path);
    static bool write_atomic(const std::filesystem::path& path, const JsonValue& doc);

    std::mutex m_mutex;
    JsonValue m_root;
    std::filesystem::path m_custom_path;
    std::vector<std::string> m_owned_sections;
};

} // namespace hub::config
