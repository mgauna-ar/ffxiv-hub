#pragma once

#include "hub/types.hpp"
#include "common/config/json.hpp"
#include <string>
#include <string_view>
#include <filesystem>
#include <mutex>
#include <optional>
#include <type_traits>
#include <vector>

namespace hub::config {

/// The config.json document one process holds.
///
/// One locking rule: every member takes m_mutex, and nothing hands out a
/// reference into the document. Reads return copies and never insert a key;
/// writes change memory only, until save().
class ConfigManager {
public:
    /// The process-wide document. Separate instances exist only so tests can
    /// stand in for the app and the payload writing one file.
    static ConfigManager& instance() noexcept {
        static ConfigManager s_instance;
        return s_instance;
    }

    ConfigManager() = default;
    ~ConfigManager() = default;
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;

    /// Absolute path to config.json (%APPDATA%/ffxiv-hub/config.json)
    [[nodiscard]] std::filesystem::path get_config_path() const;

    /// What reset_to_defaults() restores, and what the document starts from: the
    /// keys already in memory are laid over it. This file knows no plugin's keys;
    /// the app and the payload build the document from each plugin's key table.
    void set_defaults(JsonValue defaults);
    [[nodiscard]] JsonValue defaults() const;

    /// Rebuilds the document as config.json laid over the defaults. Keys set in
    /// memory and never saved are dropped, which is what "Reload from disk" means.
    /// False, with the document untouched, when the file is missing or unreadable.
    bool load();

    /// Writes the document to disk through a temp file and a rename. With owned
    /// sections set, only those sections' keys are merged into the file as it is
    /// now on disk. A failure is logged when it starts and when it clears.
    bool save();

    /// Replaces the in-memory document with the defaults. Does not touch disk.
    void reset_to_defaults();

    /// Sections this process writes back. Empty (the default) writes the whole
    /// document. The payload scopes itself to its plugin sections so its stale
    /// copy of app-only keys never overwrites what the app saved since.
    void set_owned_sections(std::vector<std::string> sections);

    // Reads. Each is a copy taken under the lock.

    /// The whole document.
    [[nodiscard]] JsonValue document() const;
    /// One section, or an empty object when it is absent or not an object.
    [[nodiscard]] JsonValue section(std::string_view name) const;
    /// One key, or nullopt when it or its section is absent.
    [[nodiscard]] std::optional<JsonValue> value(std::string_view section, std::string_view key) const;

    /// A key read as bool, int, float, double or std::string. `fallback` when
    /// the key is absent or holds another type.
    template <typename T>
    [[nodiscard]] T get(std::string_view section, std::string_view key, T fallback) const {
        const auto v = value(section, key);
        if (!v) return fallback;
        if constexpr (std::is_same_v<T, bool>) {
            return v->as_bool(fallback);
        } else if constexpr (std::is_same_v<T, int>) {
            return v->as_int(fallback);
        } else if constexpr (std::is_same_v<T, float>) {
            return v->as_float(fallback);
        } else if constexpr (std::is_same_v<T, double>) {
            return v->as_double(fallback);
        } else {
            static_assert(std::is_same_v<T, std::string>, "get reads a bool, an int, a float, a double or a string");
            return v->as_string(fallback);
        }
    }

    // Writes. Memory only; save() persists them.

    /// Sets one key, creating its section.
    void set(std::string_view section, std::string_view key, JsonValue value);
    /// Sets each key of the object `keys` in `section`, leaving its others alone.
    void merge_section(std::string_view section, const JsonValue& keys);
    /// Replaces a whole section.
    void set_section(std::string_view section, JsonValue object);

    /// Clamps an overlay window rectangle to keep it visible inside screen boundaries
    static Rect clamp_geometry_to_screen(
        const Rect& rect,
        float screen_width = 1920.0f,
        float screen_height = 1080.0f
    ) noexcept;

    void set_custom_path_for_testing(std::filesystem::path path) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_custom_path = std::move(path);
    }

private:
    [[nodiscard]] std::filesystem::path config_path_locked() const;
    [[nodiscard]] JsonValue& section_locked(std::string_view name);
    static std::optional<JsonValue> read_disk_document(const std::filesystem::path& path);
    /// Empty on success, else what went wrong.
    static std::string write_atomic(const std::filesystem::path& path, const JsonValue& doc);

    mutable std::mutex m_mutex;
    JsonValue m_root{JsonValue::ObjectType{}};
    JsonValue m_defaults{JsonValue::ObjectType{}};
    std::filesystem::path m_custom_path;
    std::vector<std::string> m_owned_sections;
    /// The last save failed, so the next failure is not logged again.
    bool m_save_failing{false};
};

} // namespace hub::config
