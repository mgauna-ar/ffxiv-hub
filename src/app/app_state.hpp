#pragma once

#include "hub/types.hpp"
#include "common/config/config_manager.hpp"
#include "common/ipc/pipe_server.hpp"
#include "common/os/network_monitor.hpp"
#include "meter/encounter_engine.hpp"
#include "meter/combatant_registry.hpp"
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <atomic>
#include <memory>
#include <chrono>

namespace hub::app {

enum class DesktopView : uint8_t {
    Dashboard,
    CombatMeter,
    LatencyMitigator,
    Settings
};

enum class ConnectionState : uint8_t {
    WaitingForGame,
    Injecting,
    InjectedWaitingPipe,
    Connected
};

struct RegisteredPluginInfo {
    PluginId id;
    std::string name;
    std::string version;
    std::string description;
    bool active{true};
    DesktopView view{DesktopView::Dashboard};
};

/**
 * @brief Central Application State for FFXIV Hub Desktop Manager.
 *
 * Coordinates configuration, background supervisor for game injection,
 * multiplexed IPC packet ingestion, and synchronization with the desktop UI.
 */
class AppState {
public:
    AppState();
    ~AppState();

    AppState(const AppState&) = delete;
    AppState& operator=(const AppState&) = delete;

    /// Initializes config, pipe server, and plugin subsystems
    bool initialize();

    /// Shuts down subsystems and flushes config
    void shutdown();

    /// Periodically called on UI/Main thread to supervise game process and connection
    void update();

    // Navigation
    [[nodiscard]] DesktopView current_view() const noexcept { return m_current_view; }
    void set_current_view(DesktopView view) noexcept { m_current_view = view; }

    // Connection & Supervisor
    [[nodiscard]] ConnectionState connection_state() const noexcept { return m_connection_state.load(); }
    [[nodiscard]] uint32_t game_pid() const noexcept { return m_game_pid.load(); }
    [[nodiscard]] bool is_connected() const noexcept;
    [[nodiscard]] bool is_access_denied() const noexcept { return m_access_denied.load(); }
    [[nodiscard]] std::string connection_status_string() const;

    // Config Manager
    [[nodiscard]] config::ConfigManager& config_manager() noexcept { return config::ConfigManager::instance(); }
    [[nodiscard]] const config::ConfigManager& config_manager() const noexcept { return config::ConfigManager::instance(); }

    // Pipe Server
    [[nodiscard]] ipc::PipeServer& pipe_server() noexcept { return m_pipe_server; }
    [[nodiscard]] const ipc::PipeServer& pipe_server() const noexcept { return m_pipe_server; }

    // Registered Plugins metadata (dynamic list for Dashboard)
    [[nodiscard]] const std::vector<RegisteredPluginInfo>& registered_plugins() const noexcept {
        return m_plugins;
    }

    /// Config section a plugin's settings live under, or nullptr for an id that
    /// owns none. The only place the id -> section mapping is written down.
    [[nodiscard]] static const char* plugin_config_section(PluginId id) noexcept;

    /// Master switch for a plugin: persists the choice, mirrors it into the
    /// registered-plugin list, and tells the payload to stop or resume.
    void set_plugin_enabled(PluginId id, bool enabled);
    [[nodiscard]] bool is_plugin_enabled(PluginId id) const noexcept;

    /// Puts every setting back to its default, on disk, in the app and in-game.
    void reset_config();
    [[nodiscard]] size_t enabled_plugin_count() const noexcept;

    // ==========================================
    // Combat Meter Plugin Integration
    // ==========================================
    // No raw engine/registry accessor: every read goes through the snapshot
    // getters below so it cannot skip m_combat_mutex.
    [[nodiscard]] meter::EncounterSummary get_live_summary();
    /// Header-only archive listing. Prefer this over get_pull_history() for
    /// anything rendered per frame: a full summary carries every combatant's
    /// per-action breakdown.
    [[nodiscard]] std::vector<meter::PullHistoryEntry> get_pull_history_index();
    [[nodiscard]] std::optional<meter::EncounterSummary> get_pull(size_t index);
    [[nodiscard]] std::vector<meter::EncounterSummary> get_pull_history();
    void reset_encounter();
    void clear_pull_history();

    // Combat Overlay in-game controls
    void send_combat_overlay_party_only(bool party_only);
    void send_combat_show_bars(bool show);
    void send_combat_hide_inactive(bool hide);
    void send_combat_refresh_interval(uint32_t ms);
    void send_combat_inactivity_timeout(float seconds);
    void send_combat_column_share(bool show);
    void send_combat_column_crit(bool show);
    void send_combat_column_dh(bool show);
    void send_combat_column_cdh(bool show);
    void send_combat_overlay_metric(uint32_t metric);
    /// Death, buff and debuff tracking (the payload's vitals polling).
    void send_combat_track_vitals(bool enabled);
    void send_combat_reset_stats();
    void send_combat_reset_overlay_geometry();
    void send_combat_end_encounter();

    // ==========================================
    // Latency Mitigator Plugin Integration
    // ==========================================
    struct MitigatorMetrics {
        float latest_measured_rtt_ms{0.0f};
        float latest_smoothed_rtt_ms{0.0f};
        float latest_jitter_ms{0.0f};
        float total_delay_reduced_ms{0.0f};
        uint64_t total_actions_mitigated{0};
        uint64_t spike_filtered_count{0};
        uint64_t floor_clamp_count{0};
    };

    [[nodiscard]] MitigatorMetrics get_mitigator_metrics();
    [[nodiscard]] std::vector<ipc::MitigatorTelemetryPayload> get_recent_telemetry(size_t max_count = 300);

    // Latency Mitigator in-game controls
    void send_mitigator_target_ping(float target_ping_ms);
    void send_mitigator_min_lock(float min_lock_ms);
    void send_mitigator_spike_multiplier(float mult);
    void send_mitigator_dry_run(bool dry_run);
    void send_mitigator_hud_display_mode(uint32_t mode);
    void send_mitigator_enabled(bool enabled);
    void send_mitigator_reset_stats();
    void send_mitigator_reset_overlay_geometry();

    /// Tells the payload to re-read config.json. Without it the payload keeps its
    /// own copy and overwrites hand edits on its next autosave.
    void send_reload_config();

    /// Asks the payload to unhook and unload without killing the game.
    void send_unhook_and_exit();

    /// Independent ICMP ping to the game server, measured from the desktop process
    /// (not the in-game hooks), so it's available immediately on login.
    [[nodiscard]] double network_ping_ms() const noexcept { return m_network_monitor.get_current_ping_ms(); }
    void send_network_ping(float ping_ms);

    /// Hook state as last reported by the payload. "Connected" alone only means
    /// the pipe came up, which is not the same as the game hooks being live.
    [[nodiscard]] bool hooks_installed() const noexcept { return m_hooks_installed.load(); }
    [[nodiscard]] std::string payload_status_message() const;

    /// Last overlay geometry the payload reported, so the app can show where an
    /// overlay actually sits after an in-game drag.
    [[nodiscard]] std::optional<ipc::OverlayGeometryPayload> overlay_geometry(PluginId id) const;
    void send_overlay_position(PluginId id, float x, float y);

    /// Every overlay answers to the same command IDs, so the shared settings
    /// controls send through here rather than a wrapper per plugin per control.
    void send_overlay_command(PluginId id, CommandId cmd, uint32_t param_uint = 0,
                              float param_float = 0.0f, float param_float2 = 0.0f);

    /// Coarse game state the payload last reported, as a hub::GameStateFlag
    /// bitmask. Zero until the payload connects, or if its scan failed.
    [[nodiscard]] uint32_t game_state_flags() const noexcept { return m_game_state_flags.load(); }

private:
    void register_ipc_callbacks();
    void apply_config_to_mirror_engine();

    /// Folds geometry pushed by the payload into the config root. The app loads
    /// config.json once and rewrites the whole document on every save, so
    /// without this a settings toggle or app exit writes back the position the
    /// overlay had at app startup and undoes an in-game drag.
    void mirror_geometry_to_config();
    void check_game_process();

    DesktopView m_current_view{DesktopView::Dashboard};
    ipc::PipeServer m_pipe_server;

    std::atomic<ConnectionState> m_connection_state{ConnectionState::WaitingForGame};
    std::atomic<uint32_t> m_game_pid{0};
    std::atomic<bool> m_access_denied{false};
    std::atomic<bool> m_hooks_installed{false};
    std::atomic<uint32_t> m_game_state_flags{0};
    std::atomic<uint64_t> m_last_heartbeat_ms{0};
    mutable std::mutex m_status_mutex;
    std::string m_payload_status_message;
    std::optional<ipc::OverlayGeometryPayload> m_combat_geometry;
    std::optional<ipc::OverlayGeometryPayload> m_latency_geometry;
    std::chrono::steady_clock::time_point m_last_process_check{};

    os::NetworkMonitor m_network_monitor;
    double m_last_sent_ping_ms{-1.0};
    std::chrono::steady_clock::time_point m_last_ping_check{};

    std::vector<RegisteredPluginInfo> m_plugins;

    // Combat Meter Subsystem
    std::mutex m_combat_mutex;
    meter::EncounterEngine m_engine;

    // Latency Mitigator Subsystem
    std::mutex m_telemetry_mutex;
    std::deque<ipc::MitigatorTelemetryPayload> m_telemetry_history;
    MitigatorMetrics m_mitigator_metrics;
};

} // namespace hub::app
