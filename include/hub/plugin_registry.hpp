#pragma once

#include "hub/types.hpp"
#include "hub/version.hpp"
#include <array>
#include <string>
#include <vector>

namespace hub::plugins {

/// What the payload, the app and the plugins themselves agree a plugin is. Every
/// plugin's name, version and config section is written here and nowhere else.
struct PluginDescriptor {
    PluginId id;
    const char* name;
    const char* version;
    const char* description;
    /// config.json section holding the plugin's settings. The payload owns it (see
    /// ConfigManager::set_owned_sections) and the app writes only its app-only keys.
    const char* config_section;
};

inline constexpr PluginDescriptor COMBAT_METER{
    PluginId::CombatMeter,
    "Combat Meter",
    HUB_VERSION_STRING,
    "High-precision real-time DPS/HPS analytics and pull drilldowns",
    "combat_meter",
};

inline constexpr PluginDescriptor LATENCY_MITIGATOR{
    PluginId::LatencyMitigator,
    "Latency Mitigator",
    HUB_VERSION_STRING,
    "Client-side animation lock compensation & slide-cast preservation",
    "latency_mitigator",
};

/// Every plugin, in the order the app lists them. Not the payload's hook dispatch
/// order, which dllmain.cpp's registration sets.
inline constexpr std::array ALL{COMBAT_METER, LATENCY_MITIGATOR};

/// The descriptor for `id`, or nullptr for an id that is not a plugin (None, Core).
[[nodiscard]] constexpr const PluginDescriptor* find(PluginId id) noexcept {
    for (const auto& d : ALL) {
        if (d.id == id) return &d;
    }
    return nullptr;
}

/// Every plugin's config section: what the payload owns in config.json.
[[nodiscard]] inline std::vector<std::string> config_sections() {
    std::vector<std::string> sections;
    for (const auto& d : ALL) sections.emplace_back(d.config_section);
    return sections;
}

} // namespace hub::plugins
