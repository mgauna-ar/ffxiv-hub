#include "payload/hook_manager.hpp"
#include "payload/dx11_hook.hpp"
#include "hub/game_definitions.hpp"
#include "payload/overlay_host.hpp"
#include "payload/game_state_reader.hpp"
#include "payload/object_reader.hpp"
#include "payload/command_dispatcher.hpp"
#include "common/ipc/ring_buffer.hpp"
#include "common/ipc/pipe_client.hpp"
#include "common/config/config_manager.hpp"
#include "common/os/logger.hpp"
#include "meter/combat_plugin.hpp"
#include "meter/combat_overlay.hpp"
#include "mitigator/latency_plugin.hpp"
#include "mitigator/latency_overlay.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <memory>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

HMODULE g_dll_module = nullptr;
std::atomic<bool> g_shutdown_requested{false};

DWORD WINAPI PayloadMainThread(LPVOID module_handle) {
    (void)module_handle;

    // 0. Diagnostic log for this in-game payload, separate from the desktop app's
    //    own hub.log (a second process truncating/writing that same file would
    //    corrupt it) so both sides of the handshake can be inspected independently.
    {
        const auto cfg_path = hub::config::ConfigManager::instance().get_config_path();
        const std::string payload_log_path = cfg_path.has_parent_path()
            ? (cfg_path.parent_path() / "hub_payload.log").string()
            : "hub_payload.log";
        hub::os::Logger::init(payload_log_path, /*rotate=*/true);
    }
    hub::os::Logger::info("Payload thread started. Waiting for FFXIVGAME window...");

    // 1. Wait for game window readiness to avoid hooking transient splash screens
    HWND game_hwnd = nullptr;
    for (int attempts = 0; attempts < 60 && !g_shutdown_requested.load(); ++attempts) {
        game_hwnd = FindWindowW(L"FFXIVGAME", nullptr);
        if (game_hwnd && IsWindow(game_hwnd)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    if (!game_hwnd) {
        hub::os::Logger::error("Payload thread giving up: FFXIVGAME window never appeared (or shutdown was requested).");
        return 0;
    }
    hub::os::Logger::info("Game window found. Initializing plugins and hooks...");

    // 2. Initialize plugins and overlays. Pure C++ construction, no game memory
    //    touched yet.
    auto latency_plugin = std::make_shared<hub::mitigator::LatencyPlugin>();
    latency_plugin->initialize();

    auto combat_plugin = std::make_shared<hub::meter::CombatPlugin>();
    combat_plugin->initialize();

    auto latency_overlay = std::make_shared<hub::mitigator::LatencyOverlay>();
    auto combat_overlay = std::make_shared<hub::meter::CombatOverlay>(&combat_plugin->engine());

    latency_plugin->set_overlay(latency_overlay.get());
    combat_plugin->set_overlay(combat_overlay.get());

    // Published by the orchestration loop below, read by the render thread when
    // an overlay evaluates its visibility conditions.
    hub::GameStateProvider game_state;
    combat_plugin->set_game_state(&game_state);
    hub::payload::OverlayHost::instance().set_game_state(&game_state);

    hub::payload::OverlayHost::instance().register_overlay(latency_overlay);
    hub::payload::OverlayHost::instance().register_overlay(combat_overlay);

    // 3. Bootstrap plugin config from disk now that overlays are wired, so
    //    persisted desktop settings apply in-game without waiting for a live command.
    hub::config::ConfigManager::instance().load();
    combat_plugin->deserialize_config(hub::config::ConfigManager::instance().root()["combat_meter"]);
    latency_plugin->deserialize_config(hub::config::ConfigManager::instance().root()["latency_mitigator"]);

    // 4. Connect to the desktop app's pipe before touching any game memory below,
    //    so a hook/sigscan failure still leaves the app able to report it.
    auto pipe_client = std::make_unique<hub::ipc::PipeClient>();
    combat_plugin->set_ring_buffer(&pipe_client->ring_buffer());
    latency_plugin->set_ring_buffer(&pipe_client->ring_buffer());

    hub::payload::CommandDispatchTargets dispatch_targets{
        combat_plugin.get(), combat_overlay.get(), latency_plugin.get(), latency_overlay.get(),
        &g_shutdown_requested
    };
    pipe_client->set_command_handler([&dispatch_targets](const hub::ipc::CommandPayload& cmd) {
        hub::payload::dispatch_command(dispatch_targets, cmd);
    });

    hub::os::Logger::info("Connecting to desktop app pipe (" + std::string(hub::ipc::DEFAULT_PIPE_NAME) + ")...");
    const bool connected_initially = pipe_client->connect(10000);
    hub::os::Logger::info(std::string("Initial pipe connect -> ") + (connected_initially ? "connected" : "not connected yet, will keep retrying"));

    // 5. Initialize HookManager and attach consumers
    auto& hook_mgr = hub::payload::HookManager::instance();
    // Registration order is dispatch order. Mitigator before meter, per
    // AGENTS.md invariant 1.
    if (!hook_mgr.register_consumer(latency_plugin.get())) {
        hub::os::Logger::warn("Failed to register the latency mitigator as a hook consumer");
    }
    if (!hook_mgr.register_consumer(combat_plugin.get())) {
        hub::os::Logger::warn("Failed to register the combat meter as a hook consumer");
    }
    hook_mgr.set_ring_buffer(&pipe_client->ring_buffer());

    hub::os::Logger::info("Installing game hooks...");
    const bool hooks_installed = hook_mgr.install();
    hub::os::Logger::info(
        "HookManager::install() -> " + std::string(hooks_installed ? "ok" : "FAILED") +
        " (" + std::to_string(hook_mgr.active_hook_count()) +
        "/" + std::to_string(hub::game::definitions::TOTAL_AVAILABLE_HOOKS) + " hooks active)" +
        " [" + hook_mgr.last_error() + "]"
    );
    hub::os::Logger::info(
        std::string("ActionManager instance -> ") + (hook_mgr.action_manager() ? "resolved" : "NOT FOUND (mitigation waits for first action)")
    );

    // 6. Initialize ObjectReader
    auto object_reader = std::make_unique<hub::payload::ObjectReader>(&pipe_client->ring_buffer());
    const bool object_reader_ok = object_reader->initialize();
    hub::os::Logger::info(std::string("ObjectReader::initialize() -> ") + (object_reader_ok ? "ok" : "FAILED"));

    // 6b. Resolve the Conditions array that drives overlay visibility conditions.
    hub::payload::GameStateReader game_state_reader;
    const bool game_state_ok = game_state_reader.initialize();
    hub::os::Logger::info(
        std::string("GameStateReader::initialize() -> ") +
        (game_state_ok ? "ok" : std::string("FAILED (") + game_state_reader.last_error() + ")"));

    combat_plugin->set_actor_resolver(
        [reader = object_reader.get(), plugin = combat_plugin.get()](uint32_t entity_id) {
            plugin->engine().with_registry([&](hub::meter::CombatantRegistry& registry) {
                reader->inspect_and_sync_actor(entity_id, &registry);
            });
        }
    );

    combat_plugin->set_actor_object_resolver(
        [reader = object_reader.get(), plugin = combat_plugin.get()](const void* character) {
            plugin->engine().with_registry([&](hub::meter::CombatantRegistry& registry) {
                reader->inspect_and_sync_actor_direct(character, &registry);
            });
        }
    );

    combat_plugin->set_hp_resolver(
        [reader = object_reader.get(), plugin = combat_plugin.get()](uint32_t entity_id, uint32_t& current_hp, uint32_t& max_hp) {
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

    // 7. Install DirectX 11 Hook (Present & ResizeBuffers)
    const bool dx11_ok = hub::payload::Dx11Hook::instance().install();
    hub::os::Logger::info(std::string("Dx11Hook::install() -> ") + (dx11_ok ? "ok" : "FAILED"));
    hub::os::Logger::info("Payload initialization complete, entering orchestration loop.");

    // 8. Background orchestration loop
    auto last_party_sync = std::chrono::steady_clock::now();
    auto last_reconnect_attempt = std::chrono::steady_clock::now();
    auto last_config_save = std::chrono::steady_clock::now();
    auto last_meter_report = std::chrono::steady_clock::now();
    auto last_heartbeat = std::chrono::steady_clock::now();
    auto last_geometry_sync = std::chrono::steady_clock::now();
    auto last_game_state_push = std::chrono::steady_clock::now();
    uint32_t last_game_state_flags = 0;
    const auto payload_start = std::chrono::steady_clock::now();
    uint32_t heartbeat_sequence = 0;
    size_t last_combatant_count = 0;
    bool prev_connected = pipe_client->is_connected();
    bool latency_overlay_prev_visible = latency_overlay->is_visible();
    latency_plugin->set_connected(prev_connected);
    if (!prev_connected) {
        latency_overlay->set_visible(false);
    }

    while (!g_shutdown_requested.load() && !hub::payload::Dx11Hook::is_shutting_down()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        auto now = std::chrono::steady_clock::now();

        // Loader-disconnect safety: hide the latency HUD and stop applying
        // mitigation write-backs while nobody is listening, without touching the
        // user's configured dry_run preference. Restore prior visibility on reconnect.
        const bool connected = pipe_client->is_connected();
        if (connected != prev_connected) {
            hub::os::Logger::info(std::string("Pipe connection state changed -> ") + (connected ? "connected" : "disconnected"));
            latency_plugin->set_connected(connected);
            if (connected) {
                // Whoever just connected has none of the names, party or zone
                // this session already published. The caches suppress repeats,
                // so without this they would never be sent again.
                object_reader->invalidate_cache();
            }
            if (!connected) {
                latency_overlay_prev_visible = latency_overlay->is_visible();
                latency_overlay->set_visible(false);
            } else {
                // Never resurrect the HUD of a plugin the user switched off.
                latency_overlay->set_visible(latency_overlay_prev_visible &&
                                            latency_plugin->is_plugin_enabled());
            }
            prev_connected = connected;
        }

        if (!connected &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_reconnect_attempt).count() > 2000) {
            pipe_client->connect(500);
            last_reconnect_attempt = now;
        }

        // Sync party composition every 1.5 seconds
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_party_sync).count() > 1500) {
            combat_plugin->engine().with_registry([&](hub::meter::CombatantRegistry& registry) {
                object_reader->sync_party(&registry);
            });
            // Same territory id the app receives; set_zone no-ops when unchanged
            // and closes an in-flight pull when it is not.
            if (const uint16_t territory = object_reader->current_territory(); territory != 0) {
                combat_plugin->engine().set_zone(territory);
            }
            last_party_sync = now;
        }

        // One 112-byte read; 50 ms of lag on a cutscene transition is invisible.
        game_state_reader.poll(game_state);

        // Update plugin logic
        combat_plugin->update(0.05);
        latency_plugin->update(0.05);

        // Update latency overlay with smoothed RTT
        latency_overlay->update_rtt(
            latency_plugin->mitigator().get_rtt_tracker().get_smoothed_rtt_ms(),
            latency_plugin->mitigator().get_rtt_tracker().sample_count() > 0
        );

        // Report combat-meter activity so an empty overlay can be told apart from
        // an overlay that never received any action data.
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_meter_report).count() > 3000) {
            const auto summary = combat_plugin->engine().current_summary();
            if (summary.combatants.size() != last_combatant_count) {
                hub::os::Logger::info(
                    "CombatMeter: " + std::to_string(summary.combatants.size()) + " combatant(s), " +
                    std::to_string(static_cast<int>(summary.state)) + " state, " +
                    std::to_string(static_cast<uint64_t>(summary.total_dps)) + " total dps"
                );
                last_combatant_count = summary.combatants.size();
            }
            last_meter_report = now;
        }

        // Heartbeat plus hook state, so the desktop app's "Connected" reflects
        // whether the hooks are actually installed rather than only that the
        // pipe came up.
        if (connected &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_heartbeat).count() > 1000) {
            hub::ipc::HeartbeatPayload hb{};
            hb.timestamp_ms = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            hb.sequence = ++heartbeat_sequence;
            hb.uptime_seconds = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::seconds>(now - payload_start).count());
            auto hb_packet = hub::ipc::serialize_typed_packet(
                hub::PluginId::Core, hub::MessageType::Heartbeat, hb.sequence, hb);
            pipe_client->ring_buffer().push(std::move(hb_packet));

            hub::ipc::StatusPayload status{};
            status.game_pid = static_cast<uint32_t>(GetCurrentProcessId());
            status.active_plugins_mask =
                (combat_plugin ? static_cast<uint32_t>(hub::PluginId::CombatMeter) : 0u) |
                (latency_plugin ? static_cast<uint32_t>(hub::PluginId::LatencyMitigator) : 0u);
            std::string msg = hook_mgr.is_installed()
                ? std::string("Hooks installed (") + hook_mgr.last_error() + ")"
                : std::string("Hooks NOT installed: ") + hook_mgr.last_error();
            if (!game_state_reader.is_initialized()) {
                msg += std::string(" | ") + game_state_reader.last_error();
            }
            std::snprintf(status.status_message, sizeof(status.status_message), "%s", msg.c_str());
            auto status_packet = hub::ipc::serialize_typed_packet(
                hub::PluginId::Core, hub::MessageType::Status, hb.sequence, status);
            pipe_client->ring_buffer().push(std::move(status_packet));

            last_heartbeat = now;
        }

        // Game state, so the desktop app can show what the overlays are currently
        // gating on. Pushed on change, plus a keepalive so a late-connecting app
        // isn't left with a blank indicator.
        if (connected) {
            const uint32_t flags = game_state.flags();
            const bool changed = flags != last_game_state_flags;
            if (changed ||
                std::chrono::duration_cast<std::chrono::milliseconds>(now - last_game_state_push).count() > 1000) {
                hub::ipc::GameStatePayload gs{};
                gs.flags = flags;
                auto packet = hub::ipc::serialize_typed_packet(
                    hub::PluginId::Core, hub::MessageType::GameState, heartbeat_sequence, gs);
                pipe_client->ring_buffer().push(std::move(packet));
                last_game_state_flags = flags;
                last_game_state_push = now;
            }
        }

        // Overlay geometry, so the desktop app can show where an overlay actually
        // sits after the player drags it in-game.
        if (connected &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_geometry_sync).count() > 1000) {
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
                auto packet = hub::ipc::serialize_typed_packet(
                    id, hub::MessageType::OverlayGeometry, heartbeat_sequence, g);
                pipe_client->ring_buffer().push(std::move(packet));
            };
            push_geometry(hub::PluginId::CombatMeter, *combat_overlay);
            push_geometry(hub::PluginId::LatencyMitigator, *latency_overlay);
            last_geometry_sync = now;
        }

        // Persist live overlay/plugin state (position, lock, opacity, ...) to
        // config.json every few seconds. This is the only place that writes the
        // config back out, so in-game changes (dragging an overlay, the padlock
        // icon, desktop app commands) survive a restart. Throttled since it hits
        // disk and the game process can be killed outright on exit rather than
        // reaching the graceful teardown path below.
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_config_save).count() > 5000) {
            auto& config_mgr = hub::config::ConfigManager::instance();
            combat_plugin->serialize_config(config_mgr.root()["combat_meter"]);
            latency_plugin->serialize_config(config_mgr.root()["latency_mitigator"]);
            config_mgr.save();
            last_config_save = now;
        }
    }

    // Game process exiting (not just an explicit unload) - don't touch DirectX,
    // MinHook, threads, or disk from here. The OS reclaims everything.
    if (hub::payload::Dx11Hook::is_shutting_down()) {
        return 0;
    }

    // Graceful teardown when explicit unload is requested while the game keeps running
    hub::os::Logger::info("Payload shutting down.");
    {
        auto& config_mgr = hub::config::ConfigManager::instance();
        combat_plugin->serialize_config(config_mgr.root()["combat_meter"]);
        latency_plugin->serialize_config(config_mgr.root()["latency_mitigator"]);
        config_mgr.save();
    }
    hook_mgr.uninstall();
    hub::payload::Dx11Hook::instance().uninstall();
    pipe_client->disconnect();

    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
        case DLL_PROCESS_ATTACH:
            g_dll_module = hModule;
            DisableThreadLibraryCalls(hModule);
            CreateThread(nullptr, 0, PayloadMainThread, hModule, 0, nullptr);
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
