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
    Command             = 0x0005,
    StatusNotification  = 0x0006,
    Status              = StatusNotification,
    OverlayGeometry     = 0x0007,
    GameState           = 0x0008,

    // Latency Mitigator Messages (0x0100 - 0x01FF)
    MitigatorTelemetry  = 0x0101,

    // Combat Meter Messages (0x0200 - 0x02FF)
    CombatActionEffect  = 0x0201,
    CombatAction        = CombatActionEffect,
    CombatStatusTick    = 0x0202,
    CombatActorInfo     = 0x0204,
    CombatPartySync     = 0x0205,
    CombatControl       = 0x0206,
    CombatStatusList    = 0x0207,
    CombatLifeEvent     = 0x0208,
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
    LockOverlay         = 8,
    ClickThrough        = 9,
    /// Legacy alias kept for wire compatibility: toggles HideCondition::OutOfCombat
    AutoHide            = 10,
    FilterPartyOnly     = 11,
    SetOpacity          = 12,
    SetScale            = 13,
    SetTargetPing       = 14,
    SetMinLock          = 15,
    SetSpikeMultiplier  = 16,
    ToggleDryRun        = 17,
    UpdateNetworkPing   = 18,
    SetOverlayMode      = 19,
    SetMitigationEnabled = 20,
    ResetStats          = 21,
    ResetOverlayGeometry = 22,
    SetShowBars         = 23,
    SetHideInactive     = 24,
    SetRefreshInterval  = 25,
    SetInactivityTimeout = 26,
    SetColumnShare      = 27,
    SetColumnCrit       = 28,
    SetColumnDh         = 29,
    SetColumnCdh        = 30,
    EndEncounter        = 31,
    SetOverlayPosition  = 32,
    UnhookAndExit       = 33,
    /// param_uint is a hub::ui::HideCondition bitmask
    SetHideConditions   = 34,
    /// param_uint is a hub::meter::MeterMetric
    SetMeterMetric      = 35,
    /// Master switch for a whole plugin: hooks, telemetry and overlay all stop
    SetPluginEnabled    = 36,
    /// param_uint 0/1: the combat meter's death and status polling
    SetVitalsTracking   = 37,
    /// param_uint is a hub::meter::DpsMetric
    SetDpsMetric        = 38,
};

/// 2D Floating-point rectangle coordinates for overlay geometry
struct Rect {
    float x{0.0f};
    float y{0.0f};
    float width{0.0f};
    float height{0.0f};

    constexpr bool operator==(const Rect& other) const = default;
};

} // namespace hub
