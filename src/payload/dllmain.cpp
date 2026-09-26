#include "payload/hook_manager.hpp"
#include "payload/dx11_hook.hpp"
#include "hub/game_definitions.hpp"
#include "hub/plugin_registry.hpp"
#include "payload/overlay_host.hpp"
#include "payload/game_state_reader.hpp"
#include "payload/object_reader.hpp"
#include "payload/command_dispatcher.hpp"
#include "payload/command_queue.hpp"
#include "payload/orchestration_intervals.hpp"
#include "payload/periodic.hpp"
#include "payload/status_report.hpp"
#include "common/ipc/ring_buffer.hpp"
#include "common/ipc/pipe_client.hpp"
#include "common/config/config_manager.hpp"
#include "common/os/logger.hpp"
#include "meter/combat_plugin.hpp"
#include "meter/combat_overlay.hpp"
#include "mitigator/latency_plugin.hpp"
#include "mitigator/latency_overlay.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <span>
#include <thread>
#include <memory>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

namespace intervals = hub::payload::intervals;
using hub::payload::Periodic;
using Clock = Periodic::Clock;

std::atomic<bool> g_shutdown_requested{false};

/// Enemies whose vitals are read beside the party (8) or the local player.
constexpr size_t kTrackedEnemies = 4;
/// A vitals pass slower than this is logged, at most once per kVitalsWarningInterval.
constexpr int64_t kSlowVitalsPassUs = 2000;
constexpr std::chrono::minutes kVitalsWarningInterval{1};

// Diagnostic log for this in-game payload, separate from the desktop app's own
// hub.log (a second process truncating/writing that same file would corrupt it)
// so both sides of the handshake can be inspected independently.
void init_payload_log() {
    const auto cfg_path = hub::config::ConfigManager::instance().get_config_path();
    const std::filesystem::path payload_log_path = cfg_path.has_parent_path()
        ? cfg_path.parent_path() / "hub_payload.log"
        : std::filesystem::path("hub_payload.log");
    hub::os::Logger::init(payload_log_path, /*rotate=*/true);
}

// Waits for game window readiness to avoid hooking transient splash screens.
// Null if it never appeared or an unload was requested meanwhile.
HWND wait_for_game_window() {
    for (int attempts = 0; attempts < 60 && !g_shutdown_requested.load(); ++attempts) {
        HWND game_hwnd = FindWindowW(L"FFXIVGAME", nullptr);
        if (game_hwnd && IsWindow(game_hwnd)) {
            return game_hwnd;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    return nullptr;
}

/// When each of the loop's periodic jobs last ran, over the named intervals. The
/// loop's own start counts as the last run, so none fires on the first tick.
struct Schedule {
    explicit Schedule(Clock::time_point start)
        : vitals(intervals::VITALS, start),
          party_sync(intervals::PARTY_SYNC, start),
          reconnect(intervals::RECONNECT, start),
          meter_report(intervals::METER_REPORT, start),
          heartbeat(intervals::HEARTBEAT, start),
          game_state_keepalive(intervals::GAME_STATE_KEEPALIVE, start),
          geometry_sync(intervals::GEOMETRY_SYNC, start),
          config_autosave(intervals::CONFIG_AUTOSAVE, start) {}

    Periodic vitals;
    Periodic party_sync;
    Periodic reconnect;
    Periodic meter_report;
    Periodic heartbeat;
    Periodic game_state_keepalive;
    Periodic geometry_sync;
    Periodic config_autosave;
    /// Throttles the slow-vitals warning; the first slow pass is logged at once.
    Periodic vitals_warning{kVitalsWarningInterval, Clock::time_point{}};
};

/**
 * The payload thread's state: the plugins, overlays and readers it owns, built by
 * the setup steps in run() and driven by the orchestration loop.
 *
 * Members are destroyed in reverse order, as the locals they replace were: the
 * command queue outlives the pipe client, whose reader pushes into it until joined.
 */
class PayloadRuntime {
public:
    /// Sets up, runs the orchestration loop until an unload or game exit, and on
    /// an unload tears down. Returns true when the game is exiting, in which case
    /// the caller must park rather than destroy this object.
    bool run() {
        create_plugins();
        load_plugin_config();
        connect_pipe();
        init_game_readers();
        install_hooks();
        hub::os::Logger::info("Payload initialization complete, entering orchestration loop.");

        run_loop();

        if (hub::payload::Dx11Hook::is_shutting_down()) {
            return true;
        }
        teardown();
        return false;
    }

private:
    // --- Setup, in order -------------------------------------------------------

    // Plugins and overlays. Pure C++ construction, no game memory touched yet.
    void create_plugins() {
        m_latency_plugin->initialize();
        m_combat_plugin->initialize();

        m_latency_plugin->set_overlay(m_latency_overlay.get());
        m_combat_plugin->set_overlay(m_combat_overlay.get());

        // Published by the orchestration loop below, read by the render thread when
        // an overlay evaluates its visibility conditions.
        m_combat_plugin->set_game_state(&m_game_state);
        hub::payload::OverlayHost::instance().set_game_state(&m_game_state);

        hub::payload::OverlayHost::instance().register_overlay(m_latency_overlay);
        hub::payload::OverlayHost::instance().register_overlay(m_combat_overlay);
    }

    // Plugin config from disk now that overlays are wired, so persisted desktop
    // settings apply in-game without waiting for a live command. The payload writes
    // back only its plugin sections; the rest belongs to the app.
    void load_plugin_config() {
        auto& config = hub::config::ConfigManager::instance();
        config.set_owned_sections(hub::plugins::config_sections());
        config.set_defaults(hub::payload::plugin_config_defaults());
        config.load();
        m_combat_plugin->deserialize_config(config.section(hub::plugins::COMBAT_METER.config_section));
        m_latency_plugin->deserialize_config(config.section(hub::plugins::LATENCY_MITIGATOR.config_section));
    }

    // Connects to the desktop app's pipe before touching any game memory below,
    // so a hook/sigscan failure still leaves the app able to report it. The reader
    // thread only queues commands; the loop dispatches them, so it is not a fourth
    // thread reaching config, overlays and the engine.
    void connect_pipe() {
        m_pipe_client = std::make_unique<hub::ipc::PipeClient>();
        m_combat_plugin->set_ring_buffer(&m_pipe_client->ring_buffer());
        m_latency_plugin->set_ring_buffer(&m_pipe_client->ring_buffer());

        m_dispatch_targets = hub::payload::CommandDispatchTargets{
            m_combat_plugin.get(), m_combat_overlay.get(), m_latency_plugin.get(), m_latency_overlay.get(),
            &g_shutdown_requested
        };
        m_pipe_client->set_command_handler([this](const hub::ipc::CommandPayload& cmd) {
            if (m_command_queue.push(cmd)) return;
            // An unload must not be lost to a full queue. The flag is atomic, so
            // setting it here reaches nothing the loop owns.
            if (static_cast<hub::PluginId>(cmd.target_plugin_id) == hub::PluginId::Core &&
                static_cast<hub::CommandId>(cmd.command_id) == hub::CommandId::UnhookAndExit) {
                g_shutdown_requested.store(true);
            }
        });

        hub::os::Logger::info("Connecting to desktop app pipe (" + std::string(hub::ipc::DEFAULT_PIPE_NAME) + ")...");
        const bool connected_initially = m_pipe_client->connect(10000);
        hub::os::Logger::info(std::string("Initial pipe connect -> ") + (connected_initially ? "connected" : "not connected yet, will keep retrying"));
    }

    // Game memory readers and the meter's resolvers. Must precede install_hooks():
    // the detour thread reads the resolvers and ObjectReader's addresses unlocked
    // once hooks fire.
    void init_game_readers() {
        m_object_reader = std::make_unique<hub::payload::ObjectReader>(&m_pipe_client->ring_buffer());
        const bool object_reader_ok = m_object_reader->initialize();
        hub::os::Logger::info(std::string("ObjectReader::initialize() -> ") + (object_reader_ok ? "ok" : "FAILED"));

        // The Conditions array that drives overlay visibility conditions.
        const bool game_state_ok = m_game_state_reader.initialize();
        hub::os::Logger::info(
            std::string("GameStateReader::initialize() -> ") +
            (game_state_ok ? "ok" : std::string("FAILED (") + m_game_state_reader.last_error() + ")"));

        m_combat_plugin->set_actor_resolver(
            [reader = m_object_reader.get(), plugin = m_combat_plugin.get()](uint32_t entity_id) {
                plugin->engine().with_registry([&](hub::meter::CombatantRegistry& registry) {
                    reader->inspect_and_sync_actor(entity_id, &registry);
                });
            }
        );

        m_combat_plugin->set_actor_object_resolver(
            [reader = m_object_reader.get(), plugin = m_combat_plugin.get()](const void* character) {
                plugin->engine().with_registry([&](hub::meter::CombatantRegistry& registry) {
                    reader->inspect_and_sync_actor_direct(character, &registry);
                });
            }
        );

        m_combat_plugin->set_hp_resolver(
            [reader = m_object_reader.get(), plugin = m_combat_plugin.get()](uint32_t entity_id, uint32_t& current_hp, uint32_t& max_hp) {
                // Under the engine lock like the resolvers above: the orchestration
                // thread drives the same ObjectReader through sync_party.
                return plugin->engine().with_registry([&](hub::meter::CombatantRegistry&) {
                    hub::ipc::ActorInfoPacket actor{};
                    if (!reader->read_character(entity_id, actor)) return false;
                    current_hp = actor.current_hp;
                    max_hp = actor.max_hp;
                    return true;
                });
            }
        );

        // Read on the detour thread, where the game writes statuses, so no engine lock:
        // the reader touches nothing the orchestration thread owns.
        m_combat_plugin->set_status_reader(
            [reader = m_object_reader.get()](uint32_t entity_id, const void* character,
                                             std::span<hub::ipc::CombatStatusEntry> out) {
                return reader->read_attribution_statuses(entity_id, character, out);
            }
        );
    }

    // The game-function hooks with their consumers, then the DX11 hook.
    void install_hooks() {
        // Registration order is dispatch order. Mitigator before meter, per
        // AGENTS.md invariant 1.
        if (!m_hook_mgr.register_consumer(m_latency_plugin.get())) {
            hub::os::Logger::warn("Failed to register the latency mitigator as a hook consumer");
        }
        if (!m_hook_mgr.register_consumer(m_combat_plugin.get())) {
            hub::os::Logger::warn("Failed to register the combat meter as a hook consumer");
        }

        hub::os::Logger::info("Installing game hooks...");
        const bool hooks_installed = m_hook_mgr.install();
        hub::os::Logger::info(
            "HookManager::install() -> " + std::string(hooks_installed ? "ok" : "FAILED") +
            " (" + std::to_string(m_hook_mgr.active_hook_count()) +
            "/" + std::to_string(hub::game::definitions::TOTAL_AVAILABLE_HOOKS) + " hooks active)" +
            " [" + m_hook_mgr.last_error() + "]"
        );
        hub::os::Logger::info(
            std::string("ActionManager instance -> ") + (m_hook_mgr.action_manager() ? "resolved" : "NOT FOUND (mitigation waits for first action)")
        );

        // DirectX 11 (Present & ResizeBuffers). The game state is read first, so an
        // injection at the title screen draws no overlay frame.
        m_game_state_reader.poll(m_game_state);
        m_game_state.set_in_lobby(m_object_reader->in_lobby());
        const bool dx11_ok = hub::payload::Dx11Hook::instance().install();
        hub::os::Logger::info(std::string("Dx11Hook::install() -> ") + (dx11_ok ? "ok" : "FAILED"));
    }

    // --- Orchestration loop ----------------------------------------------------

    void run_loop() {
        const auto loop_start = Clock::now();
        m_schedule = Schedule(loop_start);
        m_payload_start = loop_start;
        auto last_tick = loop_start;

        m_prev_connected = m_pipe_client->is_connected();
        m_latency_plugin->set_connected(m_prev_connected);
        m_combat_plugin->set_connected(m_prev_connected);

        while (!g_shutdown_requested.load() && !hub::payload::Dx11Hook::is_shutting_down()) {
            std::this_thread::sleep_for(intervals::TICK);

            const auto now = Clock::now();
            const double elapsed_seconds = std::chrono::duration<double>(now - last_tick).count();
            last_tick = now;
            tick(now, elapsed_seconds);
        }
    }

    // One pass of the loop. The order is load-bearing where the comments say so.
    void tick(Clock::time_point now, double elapsed_seconds) {
        const bool connected = follow_connection();
        dispatch_commands();
        if (!connected && m_schedule.reconnect.due(now)) {
            m_pipe_client->connect(static_cast<uint32_t>(intervals::RECONNECT_TIMEOUT.count()));
            m_schedule.reconnect.mark(now);
        }
        // Ahead of the party sync, so a death lands in its pull before the wipe
        // check can close it.
        if (m_schedule.vitals.fire(now)) read_vitals(now);
        if (m_schedule.party_sync.fire(now)) sync_party();
        poll_game_state();
        update_plugins(elapsed_seconds);
        if (m_schedule.meter_report.fire(now)) report_meter();
        if (connected && m_schedule.heartbeat.due(now)) {
            send_heartbeat_and_status(now);
            m_schedule.heartbeat.mark(now);
        }
        if (connected) push_game_state(now);
        if (connected && m_schedule.geometry_sync.fire(now)) push_overlay_geometry();
        // Persist live overlay/plugin state (position, lock, opacity, ...) to
        // config.json every few seconds, so in-game changes (dragging an overlay,
        // the padlock icon, desktop app commands) survive a restart. Only the keys
        // each plugin serializes are merged into the file as it is now, so an
        // app-only key is never reverted. Throttled since it hits disk and
        // the game process can be killed outright on exit rather than reaching
        // the graceful teardown path below.
        if (m_schedule.config_autosave.fire(now)) {
            hub::payload::save_plugin_config(m_combat_plugin.get(), m_latency_plugin.get());
        }
    }

    // Loader-disconnect safety: suppress both overlays and stop applying
    // mitigation write-backs while nobody is listening, without touching the
    // user's dry_run or overlay visibility preferences. Returns whether the pipe
    // is connected.
    bool follow_connection() {
        const bool connected = m_pipe_client->is_connected();
        if (connected == m_prev_connected) return connected;

        hub::os::Logger::info(std::string("Pipe connection state changed -> ") + (connected ? "connected" : "disconnected"));
        m_latency_plugin->set_connected(connected);
        // The meter keeps counting but draws and queues nothing.
        m_combat_plugin->set_connected(connected);
        if (connected) {
            // Whoever just connected has none of the names, party, zone or
            // status lists this session already published. The caches
            // suppress repeats, so without this they would never be sent again.
            m_object_reader->invalidate_cache();
            m_combat_plugin->invalidate_published_vitals();
        }
        m_prev_connected = connected;
        return connected;
    }

    // Commands the app sent since the last tick, in the order it sent them.
    void dispatch_commands() {
        m_command_queue.drain([this](const hub::ipc::CommandPayload& cmd) {
            hub::payload::dispatch_command(m_dispatch_targets, cmd);
        });
        if (const uint64_t dropped = m_command_queue.dropped(); dropped != m_last_dropped_commands) {
            hub::os::Logger::warn("Command queue full: " + std::to_string(dropped - m_last_dropped_commands) +
                                  " command(s) from the app dropped");
            m_last_dropped_commands = dropped;
        }
    }

    // Deaths and status lists every intervals::VITALS. With track_vitals off
    // nothing is read; on_vitals then only clears what was published before.
    void read_vitals(Clock::time_point now) {
        const auto pass_start = Clock::now();
        size_t vitals_count = 0;
        if (m_combat_plugin->vitals_enabled()) {
            const auto enemies = m_combat_plugin->engine().tracked_enemies(kTrackedEnemies);
            vitals_count = m_object_reader->read_vitals(enemies, m_vitals_buffer);
        }
        const auto now_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(pass_start.time_since_epoch()).count());
        m_combat_plugin->on_vitals(std::span<const hub::meter::ActorVitals>(m_vitals_buffer.data(), vitals_count), now_us);

        const auto pass_us = std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - pass_start).count();
        if (pass_us > kSlowVitalsPassUs && m_schedule.vitals_warning.fire(now)) {
            hub::os::Logger::warn("Vitals pass took " + std::to_string(pass_us) + " us for " +
                                  std::to_string(vitals_count) + " actor(s)");
        }
    }

    // Party composition every intervals::PARTY_SYNC.
    void sync_party() {
        m_combat_plugin->engine().with_registry([&](hub::meter::CombatantRegistry& registry) {
            m_object_reader->sync_party(&registry);
        });
        // Same territory id the app receives, 0 included; set_zone no-ops when
        // unchanged and closes an in-flight pull on a real zone change.
        m_combat_plugin->engine().set_zone(m_object_reader->current_territory());
    }

    void poll_game_state() {
        // One 112-byte read; 50 ms of lag on a cutscene transition is invisible.
        m_game_state_reader.poll(m_game_state);
        // Every overlay stays hidden until a character is in the world.
        m_game_state.set_in_lobby(m_object_reader->in_lobby());
    }

    void update_plugins(double elapsed_seconds) {
        m_combat_plugin->update(elapsed_seconds);
        m_latency_plugin->update(elapsed_seconds);

        // Update latency overlay with smoothed RTT and what mitigation is doing
        m_latency_overlay->update_rtt(
            m_latency_plugin->mitigator().get_rtt_tracker().get_smoothed_rtt_ms(),
            m_latency_plugin->mitigator().get_rtt_tracker().sample_count() > 0
        );
        const auto mitigation_cfg = m_latency_plugin->mitigator().get_config();
        m_latency_overlay->set_mitigation_mode(mitigation_cfg.enabled, mitigation_cfg.dry_run);
    }

    // Reports combat-meter activity so an empty overlay can be told apart from
    // an overlay that never received any action data.
    void report_meter() {
        const auto summary = m_combat_plugin->engine().current_rankings();
        if (summary.combatants.size() == m_last_combatant_count) return;
        hub::os::Logger::info(
            "CombatMeter: " + std::to_string(summary.combatants.size()) + " combatant(s), " +
            std::to_string(static_cast<int>(summary.state)) + " state, " +
            std::to_string(static_cast<uint64_t>(summary.total_dps)) + " total dps"
        );
        m_last_combatant_count = summary.combatants.size();
    }

    // Heartbeat plus hook state, so the desktop app's "Connected" reflects
    // whether the hooks are actually installed rather than only that the
    // pipe came up.
    void send_heartbeat_and_status(Clock::time_point now) {
        hub::ipc::HeartbeatPayload hb{};
        hb.timestamp_ms = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        hb.sequence = ++m_heartbeat_sequence;
        hb.uptime_seconds = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::seconds>(now - m_payload_start).count());
        m_pipe_client->ring_buffer().push(hub::ipc::serialize_typed_packet(
            hub::PluginId::Core, hub::MessageType::Heartbeat, hb.sequence, hb));

        hub::payload::StatusFacts facts{};
        facts.game_pid = static_cast<uint32_t>(GetCurrentProcessId());
        facts.hooks_installed = m_hook_mgr.is_installed();
        facts.hook_detail = m_hook_mgr.last_error();
        facts.game_state_ok = m_game_state_reader.is_initialized();
        facts.game_state_error = m_game_state_reader.last_error();
        facts.status_reads_enabled = m_object_reader->status_reads_enabled();
        facts.combat_meter_enabled = m_combat_plugin->is_enabled();
        facts.latency_mitigator_enabled = m_latency_plugin->is_plugin_enabled();
        const auto status = hub::payload::build_status(facts);
        m_pipe_client->ring_buffer().push(hub::ipc::serialize_typed_packet(
            hub::PluginId::Core, hub::MessageType::Status, hb.sequence, status));
    }

    // Game state, so the desktop app can show what the overlays are currently
    // gating on, and its meter can end pulls when the game ends combat. Pushed on
    // change, plus a keepalive: a late-connecting app isn't left with a blank
    // indicator, and the app's meter takes the state as unknown after
    // EncounterEngine::kGameStateTtl without one.
    void push_game_state(Clock::time_point now) {
        const uint32_t flags = m_game_state.flags();
        const uint32_t client_flags = m_game_state.client_flags();
        const bool changed = flags != m_last_game_state_flags || client_flags != m_last_client_flags;
        if (!changed && !m_schedule.game_state_keepalive.due(now)) return;

        hub::ipc::GameStatePayload gs{};
        gs.flags = flags;
        gs.client_flags = client_flags;
        m_pipe_client->ring_buffer().push(hub::ipc::serialize_typed_packet(
            hub::PluginId::Core, hub::MessageType::GameState, m_heartbeat_sequence, gs));
        m_last_game_state_flags = flags;
        m_last_client_flags = client_flags;
        m_schedule.game_state_keepalive.mark(now);
    }

    // Overlay geometry, so the desktop app can show where an overlay actually
    // sits after the player drags it in-game.
    void push_overlay_geometry() {
        auto push_geometry = [&](hub::PluginId id, const hub::ui::OverlayBase& overlay) {
            const auto geom = overlay.get_geometry();
            hub::ipc::OverlayGeometryPayload g{};
            g.plugin_id = static_cast<uint16_t>(id);
            g.visible = overlay.is_visible() ? 1 : 0;
            g.locked = overlay.is_locked() ? 1 : 0;
            g.click_through = overlay.click_through() ? 1 : 0;
            g.pos_x = geom.x;
            g.pos_y = geom.y;
            g.width = geom.width;
            g.height = geom.height;
            g.opacity = overlay.opacity();
            g.scale = overlay.scale();
            m_pipe_client->ring_buffer().push(hub::ipc::serialize_typed_packet(
                id, hub::MessageType::OverlayGeometry, m_heartbeat_sequence, g));
        };
        push_geometry(hub::PluginId::CombatMeter, *m_combat_overlay);
        push_geometry(hub::PluginId::LatencyMitigator, *m_latency_overlay);
    }

    // --- Teardown --------------------------------------------------------------

    // Graceful teardown when explicit unload is requested while the game keeps running
    void teardown() {
        hub::os::Logger::info("Payload shutting down.");
        hub::payload::save_plugin_config(m_combat_plugin.get(), m_latency_plugin.get());
        m_hook_mgr.uninstall();
        hub::payload::Dx11Hook::instance().uninstall();
        m_pipe_client->disconnect();

        // The host outlives this thread and would keep both overlays, the combat
        // one holding a raw pointer into combat_plugin's engine. Unregistering takes
        // the host's mutex, which a frame still drawing holds, so this also waits out
        // a frame the Present drain above gave up on.
        auto& overlay_host = hub::payload::OverlayHost::instance();
        overlay_host.unregister_overlay(m_combat_overlay->overlay_id());
        overlay_host.unregister_overlay(m_latency_overlay->overlay_id());
        overlay_host.set_game_state(nullptr);
        m_latency_plugin->set_overlay(nullptr);
        m_combat_plugin->set_overlay(nullptr);
    }

    // --- State, in the order the setup steps create it -------------------------

    std::shared_ptr<hub::mitigator::LatencyPlugin> m_latency_plugin = std::make_shared<hub::mitigator::LatencyPlugin>();
    std::shared_ptr<hub::meter::CombatPlugin> m_combat_plugin = std::make_shared<hub::meter::CombatPlugin>();
    std::shared_ptr<hub::mitigator::LatencyOverlay> m_latency_overlay = std::make_shared<hub::mitigator::LatencyOverlay>();
    std::shared_ptr<hub::meter::CombatOverlay> m_combat_overlay =
        std::make_shared<hub::meter::CombatOverlay>(&m_combat_plugin->engine());
    hub::GameStateProvider m_game_state;

    hub::payload::CommandQueue m_command_queue;
    std::unique_ptr<hub::ipc::PipeClient> m_pipe_client;
    hub::payload::CommandDispatchTargets m_dispatch_targets;

    std::unique_ptr<hub::payload::ObjectReader> m_object_reader;
    hub::payload::GameStateReader m_game_state_reader;
    hub::payload::HookManager& m_hook_mgr = hub::payload::HookManager::instance();

    // The loop's own state.
    Schedule m_schedule{Clock::time_point{}};
    Clock::time_point m_payload_start{};
    std::array<hub::meter::ActorVitals, hub::game::definitions::MAX_PARTY_MEMBERS + kTrackedEnemies> m_vitals_buffer{};
    uint32_t m_last_game_state_flags = 0;
    uint32_t m_last_client_flags = 0;
    uint32_t m_heartbeat_sequence = 0;
    size_t m_last_combatant_count = 0;
    uint64_t m_last_dropped_commands = 0;
    bool m_prev_connected = false;
};

DWORD WINAPI PayloadMainThread(LPVOID module_handle) {
    (void)module_handle;

    init_payload_log();
    hub::os::Logger::info("Payload thread started. Waiting for FFXIVGAME window...");
    if (!wait_for_game_window()) {
        hub::os::Logger::error("Payload thread giving up: FFXIVGAME window never appeared (or shutdown was requested).");
        return 0;
    }
    hub::os::Logger::info("Game window found. Initializing plugins and hooks...");

    PayloadRuntime payload;
    if (payload.run()) {
        // Game process exiting (not just an explicit unload) - don't touch DirectX,
        // MinHook, threads, or disk from here. The OS reclaims everything. The
        // window may be gone while the game thread still runs its shutdown, and the
        // detours and overlays still point at the plugins `payload` owns, so park
        // instead of returning and destroying them; ExitProcess ends this thread.
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            DisableThreadLibraryCalls(hModule);
            if (HANDLE thread = CreateThread(nullptr, 0, PayloadMainThread, hModule, 0, nullptr)) {
                CloseHandle(thread);
            }
            break;

        case DLL_PROCESS_DETACH:
            // If lpReserved != nullptr, the game process itself is exiting.
            // Never call FreeLibraryAndExitThread, MH_Uninitialize(), or DirectX release.
            if (lpReserved == nullptr) {
                g_shutdown_requested.store(true);
            }
            break;

        default:
            break;
    }
    return TRUE;
}

#else // !_WIN32 - Cross-platform stub

// Dummy entry point for macOS / Linux test compilation
namespace hub::payload {
    void payload_stub_entry() {}
}

#endif
