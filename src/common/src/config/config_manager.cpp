#include "common/config/config_manager.hpp"
#include "common/os/logger.hpp"
#include "common/os/paths.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <utility>

namespace hub::config {

namespace {

/// Lays each section of `over` onto `base`: an object's keys one by one, anything
/// else whole.
void merge_document(JsonValue& base, const JsonValue& over) {
    for (const auto& [section_key, section_val] : over.as_object()) {
        auto& target = base[section_key];
        if (section_val.is_object() && target.is_object()) {
            for (const auto& [k, v] : section_val.as_object()) {
                target[k] = v;
            }
        } else {
            target = section_val;
        }
    }
}

} // namespace

std::filesystem::path ConfigManager::get_config_path() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return config_path_locked();
}

std::filesystem::path ConfigManager::config_path_locked() const {
    if (!m_custom_path.empty()) {
        return m_custom_path;
    }
    const auto dir = os::app_data_dir();
    return dir.empty() ? std::filesystem::path("config.json") : dir / "config.json";
}

void ConfigManager::set_defaults(JsonValue defaults) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_defaults = defaults.is_object() ? std::move(defaults) : JsonValue(JsonValue::ObjectType{});
    JsonValue root = m_defaults;
    merge_document(root, m_root);
    m_root = std::move(root);
}

JsonValue ConfigManager::defaults() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_defaults;
}

void ConfigManager::reset_to_defaults() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_root = m_defaults;
}

std::optional<JsonValue> ConfigManager::read_disk_document(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return std::nullopt;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return std::nullopt;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();

    auto parsed = JsonValue::parse(buffer.str());
    if (!parsed || !parsed->is_object()) {
        return std::nullopt;
    }
    return parsed;
}

bool ConfigManager::load() {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto parsed = read_disk_document(config_path_locked());
    if (!parsed) {
        return false;
    }
    merge_document(m_root, *parsed);
    return true;
}

void ConfigManager::set_owned_sections(std::vector<std::string> sections) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_owned_sections = std::move(sections);
}

bool ConfigManager::save() {
    std::filesystem::path path;
    std::string error;
    bool was_failing = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        path = config_path_locked();

        if (m_owned_sections.empty()) {
            error = write_atomic(path, m_root);
        } else {
            // The other process may have written since this one loaded, so start
            // from the file as it is now and lay only our own keys over it. A
            // missing or unreadable file falls back to the whole document so it
            // is still complete.
            JsonValue doc = read_disk_document(path).value_or(m_root);
            for (const auto& section : m_owned_sections) {
                const JsonValue& ours = std::as_const(m_root)[section];
                if (!ours.is_object()) {
                    continue;
                }
                auto& target = doc[section];
                if (!target.is_object()) {
                    target = JsonValue(JsonValue::ObjectType{});
                }
                for (const auto& [k, v] : ours.as_object()) {
                    target[k] = v;
                }
            }
            error = write_atomic(path, doc);
        }
        was_failing = m_save_failing;
        m_save_failing = !error.empty();
    }

    // Outside the lock: the logger reads the config path to find its own file.
    // Once per failure spell, since the payload retries every few seconds.
    if (!error.empty() && !was_failing) {
        os::Logger::error("Could not save " + os::to_utf8(path) + ": " + error);
    } else if (error.empty() && was_failing) {
        os::Logger::info("Saved " + os::to_utf8(path) + " again.");
    }
    return error.empty();
}

std::string ConfigManager::write_atomic(const std::filesystem::path& path, const JsonValue& doc) {
    // Write to a sibling temp file and rename over the target. The payload
    // and the desktop app both write this document, so a truncating write
    // can be observed half-finished by the other process.
    auto tmp = path;
    tmp += ".tmp";
    std::error_code ec;

    const auto dir = path.parent_path();
    if (!dir.empty() && !std::filesystem::exists(dir, ec)) {
        std::filesystem::create_directories(dir, ec);
        if (ec) return "cannot create its folder (" + ec.message() + ")";
    }

    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            return "cannot open " + os::to_utf8(tmp) + " for writing";
        }
        try {
            file << doc.stringify(2);
        } catch (...) {
            // Out of memory: the payload's autosave has no handler above it.
            file.setstate(std::ios::failbit);
        }
        file.flush();
        file.close();
        if (!file.good()) {
            std::filesystem::remove(tmp, ec);
            return "writing " + os::to_utf8(tmp) + " failed";
        }
    }

    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        const std::string reason = ec.message();
        std::filesystem::remove(tmp, ec);
        return "cannot replace it (" + reason + ")";
    }
    return {};
}

JsonValue ConfigManager::document() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_root;
}

JsonValue ConfigManager::section(std::string_view name) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const JsonValue& sec = m_root[std::string(name)];
    return sec.is_object() ? sec : JsonValue(JsonValue::ObjectType{});
}

std::optional<JsonValue> ConfigManager::value(std::string_view section, std::string_view key) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    const JsonValue& sec = m_root[std::string(section)];
    const std::string k(key);
    if (!sec.contains(k)) return std::nullopt;
    return sec[k];
}

JsonValue& ConfigManager::section_locked(std::string_view name) {
    auto& sec = m_root[std::string(name)];
    if (!sec.is_object()) {
        sec = JsonValue(JsonValue::ObjectType{});
    }
    return sec;
}

void ConfigManager::set(std::string_view section, std::string_view key, JsonValue value) {
    std::lock_guard<std::mutex> lock(m_mutex);
    section_locked(section)[std::string(key)] = std::move(value);
}

void ConfigManager::merge_section(std::string_view section, const JsonValue& keys) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& sec = section_locked(section);
    for (const auto& [k, v] : keys.as_object()) {
        sec[k] = v;
    }
}

void ConfigManager::set_section(std::string_view section, JsonValue object) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_root[std::string(section)] = object.is_object() ? std::move(object) : JsonValue(JsonValue::ObjectType{});
}

Rect ConfigManager::clamp_geometry_to_screen(
    const Rect& rect,
    float screen_width,
    float screen_height
) noexcept {
    Rect clamped = rect;

    // Minimum window constraints
    constexpr float MIN_WIDTH = 80.0f;
    constexpr float MIN_HEIGHT = 24.0f;
    constexpr float MIN_VISIBLE_MARGIN = 40.0f;

    clamped.width = std::clamp(clamped.width, MIN_WIDTH, screen_width);
    clamped.height = std::clamp(clamped.height, MIN_HEIGHT, screen_height);

    // Keep at least MIN_VISIBLE_MARGIN pixels visible on screen
    clamped.x = std::clamp(clamped.x, -clamped.width + MIN_VISIBLE_MARGIN, screen_width - MIN_VISIBLE_MARGIN);
    clamped.y = std::clamp(clamped.y, 0.0f, screen_height - MIN_VISIBLE_MARGIN);

    return clamped;
}

} // namespace hub::config
