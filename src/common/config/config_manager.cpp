#include "common/config/config_manager.hpp"
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <algorithm>

namespace hub::config {

ConfigManager::ConfigManager() {
    // Populate sensible defaults
    m_root["hub"] = JsonValue::ObjectType{
        {"start_with_windows", JsonValue(false)},
        {"minimize_to_tray", JsonValue(true)},
        {"show_notifications", JsonValue(true)},
        {"refresh_interval_ms", JsonValue(500)}
    };
    // Overlay keys are the canonical set shared by every plugin - see
    // hub::ui::serialize_overlay. A negative position means "never placed", so
    // the overlay falls back to its own default.
    m_root["combat_meter"] = JsonValue::ObjectType{
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
        {"party_only", JsonValue(false)},
        {"hide_inactive", JsonValue(false)},
        {"inactivity_timeout_seconds", JsonValue(7.0f)},
        {"overlay_metric", JsonValue(0)}
    };
    m_root["latency_mitigator"] = JsonValue::ObjectType{
        {"enabled", JsonValue(true)},
        {"dry_run", JsonValue(false)},
        {"target_ping_ms", JsonValue(15.0f)},
        {"min_animation_lock_ms", JsonValue(25.0f)},
        {"max_animation_lock_ms", JsonValue(2500.0f)},
        {"spike_multiplier", JsonValue(2.5f)},
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
        {"overlay_mode", JsonValue(0)}
    };
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

bool ConfigManager::load() {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto path = get_config_path();

    if (!std::filesystem::exists(path)) {
        return false;
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    const auto content = buffer.str();

    auto parsed = JsonValue::parse(content);
    if (!parsed || !parsed->is_object()) {
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

bool ConfigManager::save() {
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto path = get_config_path();

    try {
        const auto dir = path.parent_path();
        if (!dir.empty() && !std::filesystem::exists(dir)) {
            std::filesystem::create_directories(dir);
        }

        std::ofstream file(path);
        if (!file.is_open()) {
            return false;
        }

        file << m_root.stringify(2);
        return true;
    } catch (...) {
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
