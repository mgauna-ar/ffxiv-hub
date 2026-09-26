#pragma once

#include "common/config/json.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <span>
#include <type_traits>

namespace hub::config {

/// One key of a config section: its name, and how to write it from and read it
/// into a settings struct. A table of these is the only place a plugin names its
/// keys. It writes the section (serialize_config), reads it back
/// (deserialize_config), and, written from a default-constructed struct, gives
/// the section's defaults, so the three cannot drift apart.
template <typename Settings>
struct ConfigKey {
    const char* name;
    JsonValue (*write)(const Settings&);
    /// Only called for a key present in the section. A value of the wrong type
    /// leaves the field as it was.
    void (*read)(const JsonValue&, Settings&);
};

namespace detail {

template <typename T>
[[nodiscard]] JsonValue to_json(const T& value) {
    if constexpr (std::is_same_v<T, bool>) {
        return JsonValue(value);
    } else if constexpr (std::is_enum_v<T>) {
        return JsonValue(static_cast<double>(static_cast<std::underlying_type_t<T>>(value)));
    } else {
        static_assert(std::is_arithmetic_v<T>, "a config key holds a bool, a number or an enum");
        return JsonValue(static_cast<double>(value));
    }
}

/// `current` unless `value` is a number that fits T. Out-of-range numbers clamp,
/// so a hand-edited file cannot reach an undefined conversion.
template <typename T>
[[nodiscard]] T from_json(const JsonValue& value, T current) {
    if constexpr (std::is_same_v<T, bool>) {
        return value.as_bool(current);
    } else if constexpr (std::is_enum_v<T>) {
        using U = std::underlying_type_t<T>;
        return static_cast<T>(from_json<U>(value, static_cast<U>(current)));
    } else {
        static_assert(std::is_arithmetic_v<T>, "a config key holds a bool, a number or an enum");
        if (!value.is_number()) return current;
        const double d = value.as_double();
        if (std::isnan(d)) return current;
        constexpr double lo = static_cast<double>(std::numeric_limits<T>::lowest());
        constexpr double hi = static_cast<double>(std::numeric_limits<T>::max());
        if (d <= lo) return std::numeric_limits<T>::lowest();
        if (d >= hi) return std::numeric_limits<T>::max();
        return static_cast<T>(d);
    }
}

} // namespace detail

/// A key bound to a field of Settings, reached through one member pointer or a
/// chain of them: `field<CombatConfig, &CombatConfig::overlay, &OverlayConfig::x>`.
template <typename Settings, auto... Path>
[[nodiscard]] constexpr ConfigKey<Settings> field(const char* name) noexcept {
    return ConfigKey<Settings>{
        name,
        [](const Settings& s) { return detail::to_json((s .* ... .* Path)); },
        [](const JsonValue& v, Settings& s) {
            auto& member = (s .* ... .* Path);
            member = detail::from_json(v, member);
        },
    };
}

/// Writes every key of `keys` into `section`, leaving its other keys alone.
template <typename Settings>
void write_keys(std::span<const ConfigKey<Settings>> keys, const Settings& settings, JsonValue& section) {
    for (const auto& key : keys) {
        section[key.name] = key.write(settings);
    }
}

/// Reads the keys present in `section` into `settings`. Absent ones keep their
/// current value.
template <typename Settings>
void read_keys(std::span<const ConfigKey<Settings>> keys, const JsonValue& section, Settings& settings) {
    if (!section.is_object()) return;
    for (const auto& key : keys) {
        if (section.contains(key.name)) key.read(section[key.name], settings);
    }
}

} // namespace hub::config
