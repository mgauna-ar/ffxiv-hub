#pragma once

#include "common/config/config_manager.hpp"
#include <algorithm>
#include <string>

namespace hub::app::ui {

/// Reads a setting straight out of ConfigManager each frame, so a control always
/// shows what the payload was actually given rather than a hardcoded literal.
/// `fallback` applies when the section or key is absent.
[[nodiscard]] inline bool cfg_bool(const char* section, const char* key, bool fallback) {
    auto& root = config::ConfigManager::instance().root();
    if (!root.contains(section)) return fallback;
    const auto& sec = root[section];
    if (!sec.is_object() || !sec.contains(key)) return fallback;
    return sec[key].as_bool(fallback);
}

[[nodiscard]] inline float cfg_float(const char* section, const char* key, float fallback) {
    auto& root = config::ConfigManager::instance().root();
    if (!root.contains(section)) return fallback;
    const auto& sec = root[section];
    if (!sec.is_object() || !sec.contains(key)) return fallback;
    return static_cast<float>(sec[key].as_double(fallback));
}

[[nodiscard]] inline int cfg_int(const char* section, const char* key, int fallback) {
    auto& root = config::ConfigManager::instance().root();
    if (!root.contains(section)) return fallback;
    const auto& sec = root[section];
    if (!sec.is_object() || !sec.contains(key)) return fallback;
    return sec[key].as_int(fallback);
}

/// Writes a setting back and persists immediately: a control the user moved must
/// survive a crash or a kill from the tray, neither of which runs a clean exit.
template <typename T>
inline void cfg_store(const char* section, const char* key, T value) {
    auto& root = config::ConfigManager::instance().root();
    if (!root.contains(section) || !root[section].is_object()) {
        root[section] = config::JsonValue(config::JsonValue::ObjectType{});
    }
    root[section][key] = config::JsonValue(value);
    config::ConfigManager::instance().save();
}

} // namespace hub::app::ui
