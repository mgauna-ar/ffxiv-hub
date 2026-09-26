#pragma once

#include "common/config/config_manager.hpp"
#include <algorithm>
#include <string>
#include <type_traits>

namespace hub::app::ui {

/// Reads a setting straight out of ConfigManager each frame, so a control always
/// shows what the payload was actually given rather than a hardcoded literal.
/// `fallback` applies when the section or key is absent, and its type picks the
/// reading: bool, int or float.
template <typename T>
[[nodiscard]] inline T cfg_get(const char* section, const char* key, T fallback) {
    static_assert(std::is_same_v<T, bool> || std::is_same_v<T, int> || std::is_same_v<T, float>,
                  "cfg_get reads a bool, an int or a float");
    auto& root = config::ConfigManager::instance().root();
    if (!root.contains(section)) return fallback;
    const auto& sec = root[section];
    if (!sec.is_object() || !sec.contains(key)) return fallback;
    if constexpr (std::is_same_v<T, bool>) {
        return sec[key].as_bool(fallback);
    } else if constexpr (std::is_same_v<T, int>) {
        return sec[key].as_int(fallback);
    } else {
        return static_cast<float>(sec[key].as_double(fallback));
    }
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
