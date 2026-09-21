#pragma once

#include "hub/game_state.hpp"
#include "hub/types.hpp"
#include <cstdint>

namespace hub::config {
class JsonValue;
}

namespace hub::ui {

/// Bits mean "hide the overlay while this is true". Zero is always visible,
/// so a default config round-trips to nothing and an unknown game state can
/// only ever fail open.
enum class HideCondition : uint32_t {
    None        = 0,
    OutOfCombat = 1u << 0,
    InCombat    = 1u << 1,
    InCutscene  = 1u << 2,
    InDuty      = 1u << 3,
    OutsideDuty = 1u << 4,
    MenuOpen    = 1u << 5,
    Loading     = 1u << 6,
    InPvP       = 1u << 7, ///< Reserved; no settings control yet
};

[[nodiscard]] constexpr uint32_t to_bits(HideCondition c) noexcept {
    return static_cast<uint32_t>(c);
}

[[nodiscard]] constexpr bool has_condition(uint32_t bits, HideCondition c) noexcept {
    return (bits & to_bits(c)) != 0;
}

[[nodiscard]] constexpr uint32_t with_condition(uint32_t bits, HideCondition c, bool on) noexcept {
    return on ? (bits | to_bits(c)) : (bits & ~to_bits(c));
}

/// Everything an overlay owns that isn't specific to what it displays.
struct OverlayConfig {
    bool visible{true};
    bool locked{false};
    bool click_through{false};
    float opacity{0.85f};
    float scale{1.0f};
    /// Negative position means never placed: the overlay uses its own default.
    float x{-1.0f};
    float y{-1.0f};
    float width{0.0f};
    float height{0.0f};
    uint32_t hide_conditions{0};

    bool operator==(const OverlayConfig&) const = default;
};

/// Canonical JSON keys shared by every overlay. Writes into the plugin's own
/// config section alongside its domain keys.
void serialize_overlay(const OverlayConfig& cfg, config::JsonValue& out);

/// Missing keys fall back to `defaults`, so each overlay keeps its own default
/// geometry without this code knowing about any of them.
[[nodiscard]] OverlayConfig deserialize_overlay(const config::JsonValue& in,
                                                const OverlayConfig& defaults);

/// True when `hide_conditions` says to hide under the given game state.
///
/// Conditions sourced from the client fail open while GameStateFlag::Valid is
/// absent - a signature that breaks after a game patch must never blank an
/// overlay. Combat is exempt because it also arrives over the packet path.
[[nodiscard]] bool conditions_hide(uint32_t hide_conditions, uint32_t state_flags) noexcept;

} // namespace hub::ui
