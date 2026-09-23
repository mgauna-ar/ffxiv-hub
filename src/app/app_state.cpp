#include "app/app_state.hpp"
#include "common/ui/overlay_config.hpp"
#include "common/os/process_finder.hpp"
#include "common/os/injector.hpp"
#include "common/os/logger.hpp"
#include <algorithm>
#include <filesystem>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace hub::app {

AppState::AppState() {
    m_plugins.push_back({
        PluginId::CombatMeter,
        "Combat Meter",
        "1.0.0",
        "High-precision real-time DPS/HPS analytics and pull drilldowns",
        true,
        DesktopView::CombatMeter
    });

    m_plugins.push_back({
        PluginId::LatencyMitigator,
        "Latency Mitigator",
        "1.0.0",
        "Client-side animation lock compensation & slide-cast preservation",
        true,
        DesktopView::LatencyMitigator
    });

    register_ipc_callbacks();
}

AppState::~AppState() {
    shutdown();
}

bool AppState::initialize() {
    config::ConfigManager::instance().load();
    // The mirror engine has to close pulls on the same schedule as the in-game
    // one, or the app's history lands on different encounter boundaries.
    const auto& meter_cfg = config::ConfigManager::instance().root()["combat_meter"];
    if (meter_cfg.contains("inactivity_timeout_seconds")) {
        std::lock_guard<std::mutex> lock(m_combat_mutex);
        m_engine.set_inactivity_timeout(meter_cfg["inactivity_timeout_seconds"].as_double(
            meter::constants::DEFAULT_INACTIVITY_TIMEOUT_SECONDS));
    }
    // Mirror the persisted master switches into the plugin list so the sidebar
    // and dashboard agree with what the payload will read off disk.
    for (auto& plugin : m_plugins) {
        plugin.active = is_plugin_enabled(plugin.id);
    }

    m_pipe_server.start();
    m_network_monitor.start(0);
    return true;
}

const char* AppState::plugin_config_section(PluginId id) noexcept {
    switch (id) {
        case PluginId::CombatMeter:      return "combat_meter";
        case PluginId::LatencyMitigator: return "latency_mitigator";
        case PluginId::None:
        case PluginId::Core:             break;
    }
    return nullptr;
}

bool AppState::is_plugin_enabled(PluginId id) const noexcept {
    const char* section = plugin_config_section(id);
    if (section == nullptr) return true;
    const auto& root = config::ConfigManager::instance().root();
    if (!root.contains(section) || !root[section].is_object()) return true;
    const auto& sec = root[section];
    // "enabled" is the combat meter's original key, kept so an existing config
    // that switched it off still reads as off.
    if (sec.contains("plugin_enabled")) return sec["plugin_enabled"].as_bool(true);
    if (id == PluginId::CombatMeter && sec.contains("enabled")) {
        return sec["enabled"].as_bool(true);
    }
    return true;
}

void AppState::set_plugin_enabled(PluginId id, bool enabled) {
    const char* section = plugin_config_section(id);
    if (section == nullptr) return;

    auto& root = config::ConfigManager::instance().root();
    if (!root.contains(section) || !root[section].is_object()) {
        root[section] = config::JsonValue(config::JsonValue::ObjectType{});
    }
    root[section]["plugin_enabled"] = config::JsonValue(enabled);
    if (id == PluginId::CombatMeter) {
        // The payload still honours the legacy key, so leaving it behind would
        // switch the plugin back off on the next load.
        root[section]["enabled"] = config::JsonValue(enabled);
    }
    config::ConfigManager::instance().save();

    for (auto& plugin : m_plugins) {
        if (plugin.id == id) plugin.active = enabled;
    }

    m_pipe_server.send_command(id, CommandId::SetPluginEnabled, enabled ? 1u : 0u);
}

size_t AppState::enabled_plugin_count() const noexcept {
    size_t count = 0;
    for (const auto& plugin : m_plugins) {
        if (plugin.active) ++count;
    }
    return count;
}

void AppState::shutdown() {
    m_network_monitor.stop();
    m_pipe_server.stop();
    config::ConfigManager::instance().save();
}

void AppState::register_ipc_callbacks() {
    m_pipe_server.set_combat_action_callback([this](const ipc::CombatActionPayload& act) {
        std::lock_guard<std::mutex> lock(m_combat_mutex);
        m_engine.process_action(act);
    });

    m_pipe_server.set_combat_tick_callback([this](const ipc::CombatStatusTickPayload& tick) {
        std::lock_guard<std::mutex> lock(m_combat_mutex);
        m_engine.process_status_tick(tick);
    });

    m_pipe_server.set_combat_actor_info_callback([this](const ipc::CombatActorInfoPayload& actor) {
        std::lock_guard<std::mutex> lock(m_combat_mutex);
        m_engine.process_actor_info(actor);
    });

    m_pipe_server.set_combat_party_sync_callback([this](const ipc::CombatPartySyncPayload& party) {
        std::lock_guard<std::mutex> lock(m_combat_mutex);
        m_engine.process_party_sync(party);
    });

    m_pipe_server.set_combat_control_callback([this](const ipc::CombatControlPayload& ctrl) {
        std::lock_guard<std::mutex> lock(m_combat_mutex);
        m_engine.process_encounter_control(ctrl);
    });

    m_pipe_server.set_combat_status_list_callback([this](const ipc::CombatStatusListPayload& list) {
        std::lock_guard<std::mutex> lock(m_combat_mutex);
        m_engine.process_status_list(list);
    });

    m_pipe_server.set_combat_life_event_callback([this](const ipc::CombatLifeEventPayload& event) {
        std::lock_guard<std::mutex> lock(m_combat_mutex);
        m_engine.process_life_event(event);
    });

    m_pipe_server.set_status_callback([this](const ipc::StatusPayload& status) {
        m_hooks_installed.store(std::string_view(status.status_message).find("NOT installed") == std::string_view::npos);
        std::lock_guard<std::mutex> lock(m_status_mutex);
        m_payload_status_message.assign(
            status.status_message,
            strnlen(status.status_message, sizeof(status.status_message)));
    });

    m_pipe_server.set_heartbeat_callback([this](const ipc::HeartbeatPayload& hb) {
        m_last_heartbeat_ms.store(hb.timestamp_ms);
    });

    m_pipe_server.set_overlay_geometry_callback([this](const ipc::OverlayGeometryPayload& geom) {
        const auto id = static_cast<PluginId>(geom.plugin_id);
        std::lock_guard<std::mutex> lock(m_status_mutex);
        if (id == PluginId::CombatMeter) {
            m_combat_geometry = geom;
        } else if (id == PluginId::LatencyMitigator) {
            m_latency_geometry = geom;
        }
    });

    m_pipe_server.set_game_state_callback([this](const ipc::GameStatePayload& gs) {
        m_game_state_flags.store(gs.flags);
    });

    m_pipe_server.set_mitigator_telemetry_callback([this](const ipc::MitigatorTelemetryPayload& telem) {
        std::lock_guard<std::mutex> lock(m_telemetry_mutex);
        m_telemetry_history.push_back(telem);
        if (m_telemetry_history.size() > 500) {
            m_telemetry_history.pop_front();
        }

        m_mitigator_metrics.latest_measured_rtt_ms = telem.measured_rtt_ms;
        m_mitigator_metrics.latest_smoothed_rtt_ms = telem.smoothed_rtt_ms;
        m_mitigator_metrics.latest_jitter_ms = telem.jitter_ms;
        m_mitigator_metrics.total_delay_reduced_ms += telem.delay_reduced_ms;
        m_mitigator_metrics.total_actions_mitigated += 1;
        if (telem.spike_filtered) {
            m_mitigator_metrics.spike_filtered_count += 1;
        }
        if (telem.clamped_floor) {
            m_mitigator_metrics.floor_clamp_count += 1;
        }
    });
}

void AppState::mirror_geometry_to_config() {
    const auto fold = [](const char* section,
                         const std::optional<ipc::OverlayGeometryPayload>& geom) {
        if (!geom) return;
        auto& root = config::ConfigManager::instance().root();
        if (!root.contains(section) || !root[section].is_object()) {
            root[section] = config::JsonValue(config::JsonValue::ObjectType{});
        }
        // Geometry only. The payload also reports visible/locked/opacity/scale,
        // but those are driven by desktop app controls and this push lags them
        // by up to a second, so echoing them back would revert a toggle
        // mid-click.
        ui::store_overlay_geometry(root[section], geom->pos_x, geom->pos_y,
                                   geom->width, geom->height);
    };

    // Copy under the lock, then write. root() hands out an unguarded reference
    // and the UI reads it every frame, so the config tree is only ever touched
    // from this thread.
    std::optional<ipc::OverlayGeometryPayload> combat;
    std::optional<ipc::OverlayGeometryPayload> latency;
    {
        std::lock_guard<std::mutex> lock(m_status_mutex);
        combat = m_combat_geometry;
        latency = m_latency_geometry;
    }
    fold("combat_meter", combat);
    fold("latency_mitigator", latency);
}

void AppState::update() {
    check_game_process();
    mirror_geometry_to_config();
    {
        std::lock_guard<std::mutex> lock(m_combat_mutex);
        m_engine.update();
    }

    // Forward newly-measured network ping to the in-game HUD, throttled to once/sec.
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_ping_check).count() >= 1000) {
        m_last_ping_check = now;
        const double ping = m_network_monitor.get_current_ping_ms();
        if (is_connected() && std::abs(ping - m_last_sent_ping_ms) > 0.5) {
            m_last_sent_ping_ms = ping;
            send_network_ping(static_cast<float>(ping));
        }
    }
}

void AppState::check_game_process() {
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_process_check).count() < 1000) {
        return;
    }
    m_last_process_check = now;

    auto proc = os::ProcessFinder::find_process();
    const uint32_t pid = proc ? proc->pid : 0;
    if (pid != m_game_pid.load()) {
        m_network_monitor.set_target_pid(pid);
    }
    m_game_pid.store(pid);

    if (pid == 0) {
        m_connection_state.store(ConnectionState::WaitingForGame);
        m_access_denied.store(false);
        return;
    }

    // Check if OpenProcess failed (e.g. Access Denied / privilege mismatch)
    if (!proc->handle) {
        m_connection_state.store(ConnectionState::WaitingForGame);
        if (proc->last_error == 5) { // ERROR_ACCESS_DENIED
            m_access_denied.store(true);
            static uint32_t last_warned_pid = 0;
            if (last_warned_pid != pid) {
                last_warned_pid = pid;
                os::Logger::error("FFXIV detected (PID " + std::to_string(pid) +
                    ") but OpenProcess failed with ERROR_ACCESS_DENIED (5). Please run FFXIV Hub as Administrator.");
            }
        } else {
            m_access_denied.store(false);
            static uint32_t last_warned_pid = 0;
            if (last_warned_pid != pid) {
                last_warned_pid = pid;
                os::Logger::warn("FFXIV detected (PID " + std::to_string(pid) +
                    ") but OpenProcess failed with Win32 Error " + std::to_string(proc->last_error) + ".");
            }
        }
        return;
    }
    m_access_denied.store(false);

    if (m_pipe_server.is_connected()) {
        m_connection_state.store(ConnectionState::Connected);
#ifdef _WIN32
        CloseHandle(static_cast<HANDLE>(proc->handle));
#endif
        return;
    }

    // Process is running, but not connected yet
    auto current_state = m_connection_state.load();
    if (current_state == ConnectionState::WaitingForGame) {
#ifdef _WIN32
        // Window Readiness Guard: Ensure the main game window (FFXIVGAME) is created
        // before injecting. If injected too early on process spawn, DirectX 11 device creation hasn't
        // occurred yet or the hook binds to transient pre-boot swapchains.
        HWND h_game_wnd = FindWindowW(L"FFXIVGAME", nullptr);
        bool window_ready = false;
        if (h_game_wnd != nullptr) {
            DWORD wnd_pid = 0;
            GetWindowThreadProcessId(h_game_wnd, &wnd_pid);
            if (wnd_pid == pid) {
                window_ready = true;
            }
        }
        if (!window_ready) {
            CloseHandle(static_cast<HANDLE>(proc->handle));
            return;
        }
#endif

        if (os::DllInjector::is_payload_already_loaded(*proc)) {
            m_connection_state.store(ConnectionState::InjectedWaitingPipe);
            os::Logger::info("hub_payload.dll is already resident in FFXIV (PID: " + std::to_string(pid) + "). Awaiting IPC handshake...");
#ifdef _WIN32
            CloseHandle(static_cast<HANDLE>(proc->handle));
#endif
            return;
        }

        m_connection_state.store(ConnectionState::Injecting);
        os::Logger::info("FFXIV detected (PID: " + std::to_string(pid) + "). Injecting hub_payload.dll...");

        std::filesystem::path dll_path = "hub_payload.dll";
#ifdef _WIN32
        wchar_t exe_path_buf[MAX_PATH];
        DWORD len = GetModuleFileNameW(nullptr, exe_path_buf, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            std::filesystem::path exe_dir = std::filesystem::path(exe_path_buf).parent_path();
            std::filesystem::path candidate = exe_dir / "hub_payload.dll";
            std::error_code ec;
            if (std::filesystem::exists(candidate, ec)) {
                dll_path = candidate;
            }
        }
#endif

        os::DllInjector injector;
        bool ok = injector.inject(*proc, dll_path);
        if (ok) {
            m_connection_state.store(ConnectionState::InjectedWaitingPipe);
            os::Logger::info("hub_payload.dll injected successfully from " + dll_path.string() + ". Awaiting IPC handshake...");
        } else {
            m_connection_state.store(ConnectionState::WaitingForGame);
            os::Logger::warn("Failed to inject hub_payload.dll: " + injector.last_error());
        }
    }

#ifdef _WIN32
    CloseHandle(static_cast<HANDLE>(proc->handle));
#endif
}

bool AppState::is_connected() const noexcept {
    return m_pipe_server.is_connected();
}

std::string AppState::connection_status_string() const {
    if (m_access_denied.load()) {
        return "Access Denied (Run as Admin)";
    }
    switch (m_connection_state.load()) {
        case ConnectionState::WaitingForGame:
            return "Searching for FFXIV...";
        case ConnectionState::Injecting:
            return "Injecting Payload...";
        case ConnectionState::InjectedWaitingPipe:
            return "Connecting Pipe...";
        case ConnectionState::Connected:
            // The pipe being up says nothing about whether the detours took.
            if (!m_hooks_installed.load()) {
                return "Connected, hooks not installed (PID: " +
                       std::to_string(m_game_pid.load()) + ")";
            }
            return "Connected (PID: " + std::to_string(m_game_pid.load()) + ")";
    }
    return "Unknown";
}

std::string AppState::payload_status_message() const {
    std::lock_guard<std::mutex> lock(m_status_mutex);
    return m_payload_status_message;
}

std::optional<ipc::OverlayGeometryPayload> AppState::overlay_geometry(PluginId id) const {
    std::lock_guard<std::mutex> lock(m_status_mutex);
    if (id == PluginId::CombatMeter) return m_combat_geometry;
    if (id == PluginId::LatencyMitigator) return m_latency_geometry;
    return std::nullopt;
}

void AppState::send_overlay_position(PluginId id, float x, float y) {
    m_pipe_server.send_command(id, CommandId::SetOverlayPosition, 0, x, y);
}

void AppState::send_overlay_command(PluginId id, CommandId cmd, uint32_t param_uint,
                                    float param_float, float param_float2) {
    m_pipe_server.send_command(id, cmd, param_uint, param_float, param_float2);
}

// Combat Meter Integration
meter::EncounterSummary AppState::get_live_summary() {
    std::lock_guard<std::mutex> lock(m_combat_mutex);
    return m_engine.current_summary();
}

std::vector<meter::PullHistoryEntry> AppState::get_pull_history_index() {
    std::lock_guard<std::mutex> lock(m_combat_mutex);
    return m_engine.pull_history_index();
}

std::optional<meter::EncounterSummary> AppState::get_pull(size_t index) {
    std::lock_guard<std::mutex> lock(m_combat_mutex);
    return m_engine.pull_at(index);
}

std::vector<meter::EncounterSummary> AppState::get_pull_history() {
    std::lock_guard<std::mutex> lock(m_combat_mutex);
    return m_engine.pull_history();
}

void AppState::reset_encounter() {
    {
        std::lock_guard<std::mutex> lock(m_combat_mutex);
        m_engine.reset_current();
    }
    m_pipe_server.send_command(PluginId::CombatMeter, CommandId::ResetEncounter);
}

void AppState::clear_pull_history() {
    std::lock_guard<std::mutex> lock(m_combat_mutex);
    m_engine.clear_history();
}

void AppState::send_combat_overlay_party_only(bool party_only) {
    m_pipe_server.send_command(PluginId::CombatMeter, CommandId::FilterPartyOnly, party_only ? 1 : 0);
}

// Latency Mitigator Integration
AppState::MitigatorMetrics AppState::get_mitigator_metrics() {
    std::lock_guard<std::mutex> lock(m_telemetry_mutex);
    return m_mitigator_metrics;
}

std::vector<ipc::MitigatorTelemetryPayload> AppState::get_recent_telemetry(size_t max_count) {
    std::lock_guard<std::mutex> lock(m_telemetry_mutex);
    std::vector<ipc::MitigatorTelemetryPayload> result;
    const size_t count = (std::min)(max_count, m_telemetry_history.size());
    result.reserve(count);

    auto start_it = m_telemetry_history.end() - count;
    for (auto it = start_it; it != m_telemetry_history.end(); ++it) {
        result.push_back(*it);
    }
    return result;
}

void AppState::send_mitigator_target_ping(float target_ping_ms) {
    m_pipe_server.send_command(PluginId::LatencyMitigator, CommandId::SetTargetPing, 0, target_ping_ms);
}

void AppState::send_mitigator_min_lock(float min_lock_ms) {
    m_pipe_server.send_command(PluginId::LatencyMitigator, CommandId::SetMinLock, 0, min_lock_ms);
}

void AppState::send_mitigator_spike_multiplier(float mult) {
    m_pipe_server.send_command(PluginId::LatencyMitigator, CommandId::SetSpikeMultiplier, 0, mult);
}

void AppState::send_mitigator_dry_run(bool dry_run) {
    m_pipe_server.send_command(PluginId::LatencyMitigator, CommandId::ToggleDryRun, dry_run ? 1 : 0);
}

void AppState::send_mitigator_hud_display_mode(uint32_t mode) {
    m_pipe_server.send_command(PluginId::LatencyMitigator, CommandId::SetOverlayMode, mode);
}

void AppState::send_mitigator_enabled(bool enabled) {
    m_pipe_server.send_command(PluginId::LatencyMitigator, CommandId::SetMitigationEnabled, enabled ? 1 : 0);
}

void AppState::send_mitigator_reset_stats() {
    m_pipe_server.send_command(PluginId::LatencyMitigator, CommandId::ResetStats, 0);
}

void AppState::send_mitigator_reset_overlay_geometry() {
    m_pipe_server.send_command(PluginId::LatencyMitigator, CommandId::ResetOverlayGeometry, 0);
}

void AppState::send_reload_config() {
    m_pipe_server.send_command(PluginId::Core, CommandId::ReloadConfig, 0);
}

void AppState::send_unhook_and_exit() {
    m_pipe_server.send_command(PluginId::Core, CommandId::UnhookAndExit, 0);
}

void AppState::send_combat_end_encounter() {
    {
        std::lock_guard<std::mutex> lock(m_combat_mutex);
        m_engine.end_encounter(meter::EncounterEndReason::Manual);
    }
    m_pipe_server.send_command(PluginId::CombatMeter, CommandId::EndEncounter, 0);
}

void AppState::send_combat_show_bars(bool show) {
    m_pipe_server.send_command(PluginId::CombatMeter, CommandId::SetShowBars, show ? 1 : 0);
}

void AppState::send_combat_hide_inactive(bool hide) {
    m_pipe_server.send_command(PluginId::CombatMeter, CommandId::SetHideInactive, hide ? 1 : 0);
}

void AppState::send_combat_refresh_interval(uint32_t ms) {
    m_pipe_server.send_command(PluginId::CombatMeter, CommandId::SetRefreshInterval, ms);
}

void AppState::send_combat_inactivity_timeout(float seconds) {
    {
        std::lock_guard<std::mutex> lock(m_combat_mutex);
        m_engine.set_inactivity_timeout(static_cast<double>(seconds));
    }
    m_pipe_server.send_command(PluginId::CombatMeter, CommandId::SetInactivityTimeout, 0, seconds);
}

void AppState::send_combat_column_share(bool show) {
    m_pipe_server.send_command(PluginId::CombatMeter, CommandId::SetColumnShare, show ? 1 : 0);
}

void AppState::send_combat_column_crit(bool show) {
    m_pipe_server.send_command(PluginId::CombatMeter, CommandId::SetColumnCrit, show ? 1 : 0);
}

void AppState::send_combat_column_dh(bool show) {
    m_pipe_server.send_command(PluginId::CombatMeter, CommandId::SetColumnDh, show ? 1 : 0);
}

void AppState::send_combat_column_cdh(bool show) {
    m_pipe_server.send_command(PluginId::CombatMeter, CommandId::SetColumnCdh, show ? 1 : 0);
}

void AppState::send_combat_overlay_metric(uint32_t metric) {
    m_pipe_server.send_command(PluginId::CombatMeter, CommandId::SetMeterMetric, metric);
}

void AppState::send_combat_track_vitals(bool enabled) {
    m_pipe_server.send_command(PluginId::CombatMeter, CommandId::SetVitalsTracking, enabled ? 1 : 0);
}

void AppState::send_combat_reset_stats() {
    m_pipe_server.send_command(PluginId::CombatMeter, CommandId::ResetStats, 0);
}

void AppState::send_combat_reset_overlay_geometry() {
    m_pipe_server.send_command(PluginId::CombatMeter, CommandId::ResetOverlayGeometry, 0);
}

void AppState::send_network_ping(float ping_ms) {
    m_pipe_server.send_command(PluginId::LatencyMitigator, CommandId::UpdateNetworkPing, 0, ping_ms);
}

} // namespace hub::app
