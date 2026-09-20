#pragma once

#include <cstdint>
#include <string_view>

namespace hub {

/// Unique identifiers for hub subsystem and modular plugins
enum class PluginId : uint16_t {
    None             = 0x0000,
    Core             = 0x0001,
    CombatMeter      = 0x0002,
    LatencyMitigator = 0x0003,
};

/// IPC binary packet message types multiplexed across the single Named Pipe
enum class MessageType : uint16_t {
    // Core / Hub Lifecycle Messages (0x0001 - 0x00FF)
    Heartbeat           = 0x0001,
    HandshakeRequest    = 0x0002,
    HandshakeResponse   = 0x0003,
    ShutdownSignal      = 0x0004,
    Command             = 0x0005,
    StatusNotification  = 0x0006,

    // Latency Mitigator Messages (0x0100 - 0x01FF)
    MitigatorTelemetry  = 0x0101,
    MitigatorConfigSync = 0x0102,
    MitigatorHudGeometry= 0x0103,
    MitigatorResetStats = 0x0104,

    // Combat Meter Messages (0x0200 - 0x02FF)
    CombatActionEffect  = 0x0201,
    CombatStatusTick    = 0x0202,
    CombatEncounterEvent= 0x0203,
    CombatActorInfo     = 0x0204,
    CombatPartySync     = 0x0205,
    CombatControl       = 0x0206,
    CombatResetEncounter= 0x0207,
    CombatOverlayGeometry=0x0208,
};

/// Runtime command IDs sent from desktop manager to payload plugins
enum class CommandId : uint32_t {
    None                = 0,
    ToggleOverlay       = 1,
    SetOverlayVisible   = 2,
    SetClickThrough     = 3,
    SetLocked           = 4,
    ReloadConfig        = 5,
    ResetEncounter      = 6,
    SetDryRun           = 7,
};

/// Overall game attachment & IPC connection state
enum class ConnectionState {
    Disconnected,
    WaitingForGame,
    Attached,
    Injected,
    Connected,
    Error,
};

/// 2D Floating-point point coordinates
struct Point2D {
    float x{0.0f};
    float y{0.0f};

    constexpr bool operator==(const Point2D& other) const = default;
};

/// 2D Floating-point rectangle coordinates for overlay geometry
struct Rect {
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};

    constexpr bool operator==(const Rect& other) const = default;
};

/// Convert PluginId to user-facing name
constexpr std::string_view to_string(PluginId id) noexcept {
    switch (id) {
        case PluginId::Core:             return "Hub Core";
        case PluginId::CombatMeter:      return "Combat Meter";
        case PluginId::LatencyMitigator: return "Latency Mitigator";
        default:                         return "Unknown";
    }
}

/// Convert ConnectionState to user-facing status string
constexpr std::string_view to_string(ConnectionState state) noexcept {
    switch (state) {
        case ConnectionState::Disconnected:   return "Disconnected";
        case ConnectionState::WaitingForGame: return "Waiting for FFXIV";
        case ConnectionState::Attached:       return "Found FFXIV";
        case ConnectionState::Injected:       return "Payload Injected";
        case ConnectionState::Connected:      return "Connected";
        case ConnectionState::Error:          return "Connection Error";
        default:                              return "Unknown";
    }
}

} // namespace hub
