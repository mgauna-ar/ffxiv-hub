#include "app/app_state.hpp"
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
    m_pipe_server.start();
    m_network_monitor.start(0);
    return true;
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

void AppState::update() {
    check_game_process();
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
            return "Connected (PID: " + std::to_string(m_game_pid.load()) + ")";
    }
    return "Unknown";
}

// Combat Meter Integration
meter::EncounterSummary AppState::get_live_summary() {
    std::lock_guard<std::mutex> lock(m_combat_mutex);
    return m_engine.current_summary();
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

void AppState::send_combat_overlay_visible(bool visible) {
    m_pipe_server.send_command(PluginId::CombatMeter, CommandId::ToggleOverlay, visible ? 1 : 0);
}

void AppState::send_combat_overlay_locked(bool locked) {
    m_pipe_server.send_command(PluginId::CombatMeter, CommandId::LockOverlay, locked ? 1 : 0);
}

void AppState::send_combat_overlay_click_through(bool ct) {
    m_pipe_server.send_command(PluginId::CombatMeter, CommandId::ClickThrough, ct ? 1 : 0);
}

void AppState::send_combat_overlay_auto_hide(bool auto_hide) {
    m_pipe_server.send_command(PluginId::CombatMeter, CommandId::AutoHide, auto_hide ? 1 : 0);
}

void AppState::send_combat_overlay_opacity(float opacity) {
    m_pipe_server.send_command(PluginId::CombatMeter, CommandId::SetOpacity, 0, opacity);
}

void AppState::send_combat_overlay_scale(float scale) {
    m_pipe_server.send_command(PluginId::CombatMeter, CommandId::SetScale, 0, scale);
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

void AppState::send_mitigator_hud_visible(bool visible) {
    m_pipe_server.send_command(PluginId::LatencyMitigator, CommandId::ToggleOverlay, visible ? 1 : 0);
}

void AppState::send_mitigator_hud_locked(bool locked) {
    m_pipe_server.send_command(PluginId::LatencyMitigator, CommandId::LockOverlay, locked ? 1 : 0);
}

void AppState::send_mitigator_hud_opacity(float opacity) {
    m_pipe_server.send_command(PluginId::LatencyMitigator, CommandId::SetOpacity, 0, opacity);
}

void AppState::send_mitigator_hud_scale(float scale) {
    m_pipe_server.send_command(PluginId::LatencyMitigator, CommandId::SetScale, 0, scale);
}

void AppState::send_mitigator_hud_click_through(bool click_through) {
    m_pipe_server.send_command(PluginId::LatencyMitigator, CommandId::ClickThrough, click_through ? 1 : 0);
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
