#include "app/app_state.hpp"
#include "common/ui/overlay_config.hpp"
#include "common/os/process_finder.hpp"
#include "common/os/injector.hpp"
#include "common/os/unload_marker.hpp"
#include "common/os/logger.hpp"
#include "common/os/auto_start.hpp"
#include "hub/plugin_registry.hpp"
#include "common/os/paths.hpp"
#include "meter/combat_settings.hpp"
#include "mitigator/latency_settings.hpp"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <utility>

namespace hub::app {

namespace {
DesktopView view_for_plugin(PluginId id) noexcept {
    switch (id) {
        case PluginId::CombatMeter:      return DesktopView::CombatMeter;
        case PluginId::LatencyMitigator: return DesktopView::LatencyMitigator;
        case PluginId::None:
        case PluginId::Core:             break;
    }
    return DesktopView::Dashboard;
}
} // namespace

AppState::AppState() {
    for (const auto& plugin : plugins::ALL) {
        m_plugins.push_back({
            plugin.id,
            plugin.name,
            plugin.version,
            plugin.description,
            true,
            view_for_plugin(plugin.id)
        });
    }

    // Only the desktop draws a timeline, so only this engine keeps one.
    m_engine.set_timeline_enabled(true);
    register_ipc_callbacks();
}

AppState::~AppState() {
    shutdown();
}

config::JsonValue AppState::default_config() {
    config::JsonValue doc{config::JsonValue::ObjectType{}};
    doc["hub"] = config::JsonValue::ObjectType{
        {"start_with_windows", config::JsonValue(false)},
        {"minimize_to_tray", config::JsonValue(true)},
        {"show_notifications", config::JsonValue(true)}
    };
    // Each plugin's keys come from its own table. A plugin keeps its current value
    // for a key missing from the file, so a reset only takes in-game because
    // every key it reads is here.
    auto meter = meter::default_settings();
    const auto desktop_only = meter::desktop_default_settings();
    for (const auto& [key, value] : desktop_only.as_object()) {
        meter[key] = value;
    }
    doc[plugins::COMBAT_METER.config_section] = std::move(meter);
    doc[plugins::LATENCY_MITIGATOR.config_section] = mitigator::default_settings();
    return doc;
}

bool AppState::initialize() {
    auto& cfg = config::ConfigManager::instance();
    cfg.set_defaults(default_config());
    cfg.load();
    apply_config_to_mirror_engine();
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
    const auto* plugin = plugins::find(id);
    return plugin != nullptr ? plugin->config_section : nullptr;
}

bool AppState::is_plugin_enabled(PluginId id) const noexcept {
    const char* section = plugin_config_section(id);
    if (section == nullptr) return true;
    const auto& cfg = config::ConfigManager::instance();
    // "enabled" is the combat meter's original key, kept so an existing config
    // that switched it off still reads as off.
    if (const auto on = cfg.value(section, plugins::MASTER_SWITCH_KEY)) return on->as_bool(true);
    if (id == PluginId::CombatMeter) {
        return cfg.get(section, meter::LEGACY_ENABLED_KEY, true);
    }
    return true;
}

void AppState::set_plugin_enabled(PluginId id, bool enabled) {
    const char* section = plugin_config_section(id);
    if (section == nullptr) return;

    auto& cfg = config::ConfigManager::instance();
    cfg.set(section, plugins::MASTER_SWITCH_KEY, config::JsonValue(enabled));
    if (id == PluginId::CombatMeter) {
        // The payload still honours the legacy key, so leaving it behind would
        // switch the plugin back off on the next load.
        cfg.set(section, meter::LEGACY_ENABLED_KEY, config::JsonValue(enabled));
    }
    cfg.save();

    for (auto& plugin : m_plugins) {
        if (plugin.id == id) plugin.active = enabled;
    }

    send_command(id, CommandId::SetPluginEnabled, enabled ? 1u : 0u);
}

void AppState::apply_config_to_mirror_engine() {
    // Only this engine keeps pulls to browse, so the limit is the app's alone.
    const auto limit = config::ConfigManager::instance().value(
        plugins::COMBAT_METER.config_section, meter::PULL_HISTORY_LIMIT_KEY);
    if (limit) {
        set_pull_history_limit(limit->as_int(static_cast<int>(meter::constants::DEFAULT_HISTORY_CAPACITY)));
    }
}

void AppState::reset_config() {
    auto& cfg = config::ConfigManager::instance();
    cfg.reset_to_defaults();
    // The registry is the source of truth for auto-start, so config follows it.
    cfg.set("hub", "start_with_windows", config::JsonValue(os::AutoStart::is_enabled()));
    cfg.save();
    apply_config_to_mirror_engine();

    // The defaults carry no master switch, so turn each plugin back on explicitly.
    for (const auto& plugin : m_plugins) {
        set_plugin_enabled(plugin.id, true);
    }
    send_command(PluginId::Core, CommandId::ReloadConfig);
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

    m_pipe_server.set_combat_enemy_hp_callback([this](const ipc::CombatEnemyHpPayload& hp) {
        std::lock_guard<std::mutex> lock(m_combat_mutex);
        m_engine.process_enemy_hp(hp);
    });

    m_pipe_server.set_combat_cast_callback([this](const ipc::CombatCastPayload& cast) {
        std::lock_guard<std::mutex> lock(m_combat_mutex);
        m_engine.process_cast(cast);
    });

    m_pipe_server.set_status_callback([this](const ipc::StatusPayload& status) {
        m_hooks_installed.store(ipc::reports_hooks_installed(status));
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
            m_combat_geometry_new = true;
        } else if (id == PluginId::LatencyMitigator) {
            m_latency_geometry = geom;
            m_latency_geometry_new = true;
        }
    });

    m_pipe_server.set_game_state_callback([this](const ipc::GameStatePayload& gs) {
        m_game_state_flags.store(gs.flags);
        // 0 from a payload too old to report it, or with no character in the world.
        m_network_monitor.set_fallback_world(gs.current_world);
        // The same word the in-game engine ends its pulls on, so both close together.
        std::lock_guard<std::mutex> lock(m_combat_mutex);
        m_engine.set_game_state(gs.client_flags);
    });

    m_pipe_server.set_mitigator_telemetry_callback([this](const ipc::MitigatorTelemetryPayload& telem) {
        std::lock_guard<std::mutex> lock(m_telemetry_mutex);
        m_telemetry_history.push_back(telem);
        if (m_telemetry_history.size() > 500) {
            m_telemetry_history.pop_front();
        }

        // Casts and unmatched responses carry no round trip.
        if (telem.measured_rtt_ms > 0.0f) {
            m_mitigator_metrics.latest_measured_rtt_ms = telem.measured_rtt_ms;
        }
        m_mitigator_metrics.latest_smoothed_rtt_ms = telem.smoothed_rtt_ms;
        m_mitigator_metrics.latest_jitter_ms = telem.jitter_ms;
        // Only locks actually written count as saved: dry-run and switched-off
        // rows still report what they would have trimmed.
        if (telem.applied) {
            m_mitigator_metrics.total_delay_reduced_ms += telem.delay_reduced_ms;
            m_mitigator_metrics.total_actions_mitigated += 1;
            if (telem.clamped_floor) {
                m_mitigator_metrics.floor_clamp_count += 1;
            }
        }
        if (telem.spike_filtered) {
            m_mitigator_metrics.spike_filtered_count += 1;
        }
    });
}

void AppState::mirror_geometry_to_config() {
    const auto fold = [](const char* section,
                         const std::optional<ipc::OverlayGeometryPayload>& geom) {
        if (!geom) return;
        // Geometry only. The payload also reports visible/locked/opacity/scale,
        // but those are driven by desktop app controls and this push lags them
        // by up to a second, so echoing them back would revert a toggle
        // mid-click.
        config::JsonValue keys{config::JsonValue::ObjectType{}};
        ui::store_overlay_geometry(keys, geom->pos_x, geom->pos_y, geom->width, geom->height);
        config::ConfigManager::instance().merge_section(section, keys);
    };

    // Copy under the status lock, then write under the config's own. Only a report
    // not folded yet, so one arriving at 1 Hz is not merged again on every frame.
    std::optional<ipc::OverlayGeometryPayload> combat;
    std::optional<ipc::OverlayGeometryPayload> latency;
    {
        std::lock_guard<std::mutex> lock(m_status_mutex);
        if (std::exchange(m_combat_geometry_new, false)) combat = m_combat_geometry;
        if (std::exchange(m_latency_geometry_new, false)) latency = m_latency_geometry;
    }
    fold(plugins::COMBAT_METER.config_section, combat);
    fold(plugins::LATENCY_MITIGATOR.config_section, latency);
}

void AppState::update() {
    check_game_process();
    mirror_geometry_to_config();
    {
        std::lock_guard<std::mutex> lock(m_combat_mutex);
        m_engine.update();
    }

    // Forward newly-measured network ping to the in-game HUD, throttled to once/sec.
    // A payload that connects has none of what was sent before, and ICMP reports
    // whole milliseconds, so an unchanged ping would otherwise never reach it.
    if (!is_connected()) {
        m_last_sent_ping_ms.reset();
    }
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_ping_check).count() >= 1000) {
        m_last_ping_check = now;
        const double ping = m_network_monitor.get_current_ping_ms();
        if (is_connected() &&
            (!m_last_sent_ping_ms || std::abs(ping - *m_last_sent_ping_ms) > 0.5)) {
            m_last_sent_ping_ms = ping;
            send_command(PluginId::LatencyMitigator, CommandId::UpdateNetworkPing, 0, static_cast<float>(ping));
        }
    }
}

void AppState::rescan() {
    m_last_process_check = {};
    check_game_process();
}

void AppState::check_game_process() {
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_process_check).count() < 1000) {
        return;
    }
    m_last_process_check = now;

    // Owns the game handle find_process opened, until this check returns.
    const auto proc = os::ProcessFinder::find_process();
    const uint32_t pid = proc ? proc->pid : 0;
    if (pid != m_game_pid.load()) {
        m_network_monitor.set_target_pid(pid);
    }
    m_game_pid.store(pid);

    ConnectionObservation seen{};
    seen.process_alive = pid != 0;
    seen.handle_opened = proc && proc->handle;
    seen.pipe_connected = m_pipe_server.is_connected();
    seen.unload_requested = pid != 0 && m_unloaded_pid.load() == pid;

    if (!seen.pipe_connected) {
        // Only a live payload reports its hooks; the next one reports afresh.
        m_hooks_installed.store(false);
    }

    if (!seen.process_alive) {
        m_access_denied.store(false);
    } else if (!seen.handle_opened) {
        // OpenProcess failed (e.g. Access Denied / privilege mismatch)
        if (proc->last_error == 5) { // ERROR_ACCESS_DENIED
            m_access_denied.store(true);
            if (m_open_warned_pid != pid) {
                m_open_warned_pid = pid;
                os::Logger::error("FFXIV detected (PID " + std::to_string(pid) +
                    ") but OpenProcess failed with ERROR_ACCESS_DENIED (5). Please run FFXIV Hub as Administrator.");
            }
        } else {
            m_access_denied.store(false);
            if (m_open_warned_pid != pid) {
                m_open_warned_pid = pid;
                os::Logger::warn("FFXIV detected (PID " + std::to_string(pid) +
                    ") but OpenProcess failed with Win32 Error " + std::to_string(proc->last_error) + ".");
            }
        }
    } else {
        m_access_denied.store(false);
    }

    // Only the main game window (FFXIVGAME) of this very process counts.
    const auto window_ready = [&]() { return os::ProcessFinder::has_game_window(pid); };
    const auto payload_loaded = [&]() { return os::DllInjector::is_payload_already_loaded(*proc); };
    const auto payload_unloaded = [&]() { return os::payload_marked_unloaded(pid); };

    const ConnectionState previous = m_connection_state.load();
    const ConnectionDecision decision = decide_connection(previous, seen, window_ready, payload_loaded,
                                                          payload_unloaded);
    m_connection_state.store(decision.next);
    if (decision.next == ConnectionState::Unloaded) {
        // Found through the payload's mark after an app restart: remember it like
        // an unload sent from here, so the next checks take no module snapshot.
        m_unloaded_pid.store(pid);
    }

    if (decision.next != previous) {
        if (decision.next == ConnectionState::Reconnecting) {
            os::Logger::warn("IPC pipe to the payload dropped (PID: " + std::to_string(pid) + "). Waiting for it to reconnect...");
        } else if (decision.next == ConnectionState::Unloaded) {
            os::Logger::info("hub_payload.dll unloaded from FFXIV (PID: " + std::to_string(pid) +
                "). It stays mapped until the game restarts.");
        }
    }

    switch (decision.action) {
        case ConnectionAction::None:
            break;
        case ConnectionAction::AdoptResident:
            os::Logger::info("hub_payload.dll is already resident in FFXIV (PID: " + std::to_string(pid) + "). Awaiting IPC handshake...");
            break;
        case ConnectionAction::Inject: {
            os::Logger::info("FFXIV detected (PID: " + std::to_string(pid) + "). Injecting hub_payload.dll...");

            std::filesystem::path dll_path = "hub_payload.dll";
            if (const std::filesystem::path exe_dir = os::executable_dir(); !exe_dir.empty()) {
                const std::filesystem::path candidate = exe_dir / "hub_payload.dll";
                std::error_code ec;
                if (std::filesystem::exists(candidate, ec)) {
                    dll_path = candidate;
                }
            }

            os::DllInjector injector;
            const bool ok = injector.inject(*proc, dll_path);
            m_connection_state.store(after_injection(ok));
            if (ok) {
                os::Logger::info("hub_payload.dll injected successfully from " + os::to_utf8(dll_path) + ". Awaiting IPC handshake...");
            } else {
                os::Logger::warn("Failed to inject hub_payload.dll: " + injector.last_error());
            }
            break;
        }
    }
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
        case ConnectionState::Reconnecting:
            return "Reconnecting to payload (PID: " + std::to_string(m_game_pid.load()) + ")...";
        case ConnectionState::Unloaded:
            return "Payload unloaded. Restart the game to attach again.";
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

bool AppState::send_command(PluginId id, CommandId cmd, uint32_t param_uint,
                            float param_float, float param_float2) {
    return m_pipe_server.send_command(id, cmd, param_uint, param_float, param_float2);
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

meter::EncounterTimeline AppState::get_timeline(uint64_t encounter_id) {
    std::lock_guard<std::mutex> lock(m_combat_mutex);
    return m_engine.timeline(encounter_id);
}

void AppState::reset_encounter() {
    {
        std::lock_guard<std::mutex> lock(m_combat_mutex);
        m_engine.reset_current();
    }
    send_command(PluginId::CombatMeter, CommandId::ResetEncounter);
}

void AppState::clear_pull_history() {
    std::lock_guard<std::mutex> lock(m_combat_mutex);
    m_engine.clear_history();
}

void AppState::set_pull_history_limit(int pulls) {
    const int limit = std::clamp(pulls, meter::constants::MIN_PULL_HISTORY_LIMIT,
                                 meter::constants::MAX_PULL_HISTORY_LIMIT);
    std::lock_guard<std::mutex> lock(m_combat_mutex);
    m_engine.set_history_capacity(static_cast<size_t>(limit));
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

void AppState::clear_mitigator_stats() {
    std::lock_guard<std::mutex> lock(m_telemetry_mutex);
    m_telemetry_history.clear();
    m_mitigator_metrics = MitigatorMetrics{};
}

void AppState::send_unhook_and_exit() {
    if (send_command(PluginId::Core, CommandId::UnhookAndExit)) {
        m_unloaded_pid.store(m_game_pid.load());
    }
}

void AppState::send_combat_end_encounter() {
    {
        std::lock_guard<std::mutex> lock(m_combat_mutex);
        m_engine.end_encounter(meter::EncounterEndReason::Manual);
    }
    send_command(PluginId::CombatMeter, CommandId::EndEncounter);
}

} // namespace hub::app
