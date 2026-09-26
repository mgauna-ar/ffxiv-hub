#pragma once

#include "common/config/config_manager.hpp"
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
    return config::ConfigManager::instance().get(section, key, fallback);
}

/// Changes a setting in memory only. For a control edited over many frames, such
/// as a slider being dragged: call cfg_save() once the edit ends
/// (ImGui::IsItemDeactivatedAfterEdit), not on every frame of the drag.
template <typename T>
inline void cfg_set(const char* section, const char* key, T value) {
    config::ConfigManager::instance().set(section, key, config::JsonValue(value));
}

/// Writes config.json. ConfigManager::save logs a failure.
inline void cfg_save() {
    (void)config::ConfigManager::instance().save();
}

/// Changes a setting and persists it at once: a control the user flipped must
/// survive a crash or a kill from the tray, neither of which runs a clean exit.
template <typename T>
inline void cfg_store(const char* section, const char* key, T value) {
    cfg_set(section, key, value);
    cfg_save();
}

} // namespace hub::app::ui
