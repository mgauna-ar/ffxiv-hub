#include "common/config/config_manager.hpp"
#include "hub/plugin_registry.hpp"
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <algorithm>

namespace hub::config {

JsonValue ConfigManager::default_document() {
    // A plugin keeps its current value for a key missing from the file, so a
    // reset only takes in-game if every key it reads is listed here.
    JsonValue root{JsonValue::ObjectType{}};
    root["hub"] = JsonValue::ObjectType{
        {"start_with_windows", JsonValue(false)},
        {"minimize_to_tray", JsonValue(true)},
        {"show_notifications", JsonValue(true)},
        {"refresh_interval_ms", JsonValue(500)}
    };
    // Overlay keys are the canonical set shared by every plugin - see
    // hub::ui::serialize_overlay. A negative position means "never placed", so
    // the overlay falls back to its own default.
    root[plugins::COMBAT_METER.config_section] = JsonValue::ObjectType{
        {"overlay_visible", JsonValue(true)},
        {"overlay_x", JsonValue(-1.0f)},
        {"overlay_y", JsonValue(-1.0f)},
        {"overlay_width", JsonValue(800.0f)},
        {"overlay_height", JsonValue(480.0f)},
        {"overlay_opacity", JsonValue(0.88f)},
        {"overlay_scale", JsonValue(1.0f)},
        {"overlay_locked", JsonValue(false)},
        {"overlay_click_through", JsonValue(false)},
        {"overlay_hide_conditions", JsonValue(0)},
        {"overlay_hide_after_combat_seconds", JsonValue(5.0f)},
        {"party_only", JsonValue(false)},
        {"hide_inactive", JsonValue(false)},
        {"overlay_metric", JsonValue(0)},
        {"dps_metric", JsonValue(0)},
        // Only the app reads and writes these.
        {"desktop_dps_metric", JsonValue(0)},
        {"pull_history_limit", JsonValue(100)},
        {"show_bars", JsonValue(true)},
        {"refresh_interval_ms", JsonValue(500)},
        {"show_col_share", JsonValue(true)},
        {"show_col_crit", JsonValue(true)},
        {"show_col_dh", JsonValue(true)},
        {"show_col_cdh", JsonValue(true)},
        {"track_vitals", JsonValue(true)}
    };
    root[plugins::LATENCY_MITIGATOR.config_section] = JsonValue::ObjectType{
        {"enabled", JsonValue(true)},
        {"dry_run", JsonValue(false)},
        {"target_ping_ms", JsonValue(15.0f)},
        {"min_animation_lock_ms", JsonValue(25.0f)},
        {"max_animation_lock_ms", JsonValue(2500.0f)},
        {"spike_multiplier", JsonValue(2.5f)},
        {"rtt_sample_window", JsonValue(10)},
        {"safety_margin_ms", JsonValue(0.0f)},
        {"overlay_visible", JsonValue(true)},
        {"overlay_x", JsonValue(20.0f)},
        {"overlay_y", JsonValue(20.0f)},
        {"overlay_width", JsonValue(120.0f)},
        {"overlay_height", JsonValue(32.0f)},
        {"overlay_opacity", JsonValue(0.90f)},
        {"overlay_scale", JsonValue(1.0f)},
        {"overlay_locked", JsonValue(false)},
        {"overlay_click_through", JsonValue(false)},
        {"overlay_hide_conditions", JsonValue(0)},
        {"overlay_hide_after_combat_seconds", JsonValue(5.0f)},
        {"overlay_mode", JsonValue(0)}
    };
    return root;
}

ConfigManager::ConfigManager() : m_root(default_document()) {}

void ConfigManager::reset_to_defaults() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_root = default_document();
}

std::filesystem::path ConfigManager::get_config_path() const {
    if (!m_custom_path.empty()) {
        return m_custom_path;
    }

#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (appdata && appdata[0] != '\0') {
        return std::filesystem::path(appdata) / "ffxiv-hub" / "config.json";
    }
    return std::filesystem::path("config.json");
#else
    const char* home = std::getenv("HOME");
    if (home && home[0] != '\0') {
        return std::filesystem::path(home) / ".config" / "ffxiv-hub" / "config.json";
    }
    return std::filesystem::path("config.json");
#endif
}

std::optional<JsonValue> ConfigManager::read_disk_document(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        return std::nullopt;
    }

    std::ifstream file(path);
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
    auto parsed = read_disk_document(get_config_path());
    if (!parsed) {
        return false;
    }

    // Merge parsed fields into m_root
    for (const auto& [section_key, section_val] : parsed->as_object()) {
        if (section_val.is_object() && m_root.contains(section_key)) {
            for (const auto& [k, v] : section_val.as_object()) {
                m_root[section_key][k] = v;
            }
        } else {
            m_root[section_key] = section_val;
        }
    }

    return true;
}

void ConfigManager::set_owned_sections(std::vector<std::string> sections) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_owned_sections = std::move(sections);
}

bool ConfigManager::save() {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto path = get_config_path();

    if (m_owned_sections.empty()) {
        return write_atomic(path, m_root);
    }

    // The other process may have written since this one loaded, so start from
    // the file as it is now and lay only our own keys over it. A missing or
    // unreadable file falls back to the whole document so it is still complete.
    JsonValue doc = read_disk_document(path).value_or(m_root);
    for (const auto& section : m_owned_sections) {
        if (!m_root.contains(section) || !m_root[section].is_object()) {
            continue;
        }
        if (!doc.contains(section) || !doc[section].is_object()) {
            doc[section] = JsonValue(JsonValue::ObjectType{});
        }
        for (const auto& [k, v] : m_root[section].as_object()) {
            doc[section][k] = v;
        }
    }
    return write_atomic(path, doc);
}

bool ConfigManager::write_atomic(const std::filesystem::path& path, const JsonValue& doc) {
    try {
        const auto dir = path.parent_path();
        if (!dir.empty() && !std::filesystem::exists(dir)) {
            std::filesystem::create_directories(dir);
        }

        // Write to a sibling temp file and rename over the target. The payload
        // and the desktop app both write this document, so a truncating write
        // can be observed half-finished by the other process.
        auto tmp = path;
        tmp += ".tmp";

        {
            std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
            if (!file.is_open()) {
                return false;
            }

            file << doc.stringify(2);
            file.flush();
            file.close();

            if (!file.good()) {
                std::error_code ec;
                std::filesystem::remove(tmp, ec);
                return false;
            }
        }

        std::filesystem::rename(tmp, path);
        return true;
    } catch (...) {
        std::error_code ec;
        auto tmp = path;
        tmp += ".tmp";
        std::filesystem::remove(tmp, ec);
        return false;
    }
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
