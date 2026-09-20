#include "payload/hook_manager.hpp"
#include "payload/dx11_hook.hpp"
#include "payload/overlay_host.hpp"
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

    hub::payload::OverlayHost::instance().register_overlay(latency_overlay);
    hub::payload::OverlayHost::instance().register_overlay(combat_overlay);

    // 3. Bootstrap plugin config from disk now that overlays are wired, so
    //    persisted desktop settings apply in-game without waiting for a live command.
    hub::config::ConfigManager::instance().load();
    combat_plugin->deserialize_config(hub::config::ConfigManager::instance().root()["combat_meter"]);
    latency_plugin->deserialize_config(hub::config::ConfigManager::instance().root()["latency_mitigator"]);

    // 4. Initialize the outbound IPC client and connect to the desktop app's named
    //    pipe BEFORE touching any game memory (sigscan/MinHook detours below).
    //    Both original standalone apps this was merged from connected first, then
    //    installed hooks - matching that order here matters: if hook/sigscan
    //    installation hangs or crashes on a game patch this payload hasn't been
    //    updated for, the pipe is already up and the desktop app shows "Connected"
    //    instead of hanging at "Awaiting IPC handshake" with zero information
    //    about why.
    auto pipe_client = std::make_unique<hub::ipc::PipeClient>();
    combat_plugin->set_ring_buffer(&pipe_client->ring_buffer());
    latency_plugin->set_ring_buffer(&pipe_client->ring_buffer());

    hub::payload::CommandDispatchTargets dispatch_targets{
        combat_plugin.get(), combat_overlay.get(), latency_plugin.get(), latency_overlay.get()
    };
    pipe_client->set_command_handler([&dispatch_targets](const hub::ipc::CommandPayload& cmd) {
        hub::payload::dispatch_command(dispatch_targets, cmd);
    });

    hub::os::Logger::info("Connecting to desktop app pipe (" + std::string(hub::ipc::DEFAULT_PIPE_NAME) + ")...");
    const bool connected_initially = pipe_client->connect(10000);
    hub::os::Logger::info(std::string("Initial pipe connect -> ") + (connected_initially ? "connected" : "not connected yet, will keep retrying"));

    // 5. Initialize HookManager and attach consumers
    auto& hook_mgr = hub::payload::HookManager::instance();
    hook_mgr.set_latency_consumer(latency_plugin.get());
    hook_mgr.set_meter_consumer(combat_plugin.get());
    hook_mgr.set_ring_buffer(&pipe_client->ring_buffer());

    hub::os::Logger::info("Installing game hooks...");
    const bool hooks_installed = hook_mgr.install();
    hub::os::Logger::info(
        "HookManager::install() -> " + std::string(hooks_installed ? "ok" : "FAILED") +
        " (" + std::to_string(hook_mgr.active_hook_count()) + "/3 hooks active)"
    );

    // 6. Initialize ObjectReader
    auto object_reader = std::make_unique<hub::payload::ObjectReader>(&pipe_client->ring_buffer());
    const bool object_reader_ok = object_reader->initialize();
    hub::os::Logger::info(std::string("ObjectReader::initialize() -> ") + (object_reader_ok ? "ok" : "FAILED"));

    // 7. Install DirectX 11 Hook (Present & ResizeBuffers)
    const bool dx11_ok = hub::payload::Dx11Hook::instance().install();
    hub::os::Logger::info(std::string("Dx11Hook::install() -> ") + (dx11_ok ? "ok" : "FAILED"));
    hub::os::Logger::info("Payload initialization complete, entering orchestration loop.");

    // 8. Background orchestration loop
    auto last_party_sync = std::chrono::steady_clock::now();
    auto last_reconnect_attempt = std::chrono::steady_clock::now();
    auto last_config_save = std::chrono::steady_clock::now();
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
            if (!connected) {
                latency_overlay_prev_visible = latency_overlay->is_visible();
                latency_overlay->set_visible(false);
            } else {
                latency_overlay->set_visible(latency_overlay_prev_visible);
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
            object_reader->sync_party(&combat_plugin->engine().registry());
            last_party_sync = now;
        }

        // Update plugin logic
        combat_plugin->update(0.05);
        latency_plugin->update(0.05);

        // Update latency overlay with smoothed RTT
        latency_overlay->update_rtt(
            latency_plugin->mitigator().get_rtt_tracker().get_smoothed_rtt_ms(),
            latency_plugin->mitigator().get_rtt_tracker().sample_count() > 0
        );

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

    // Graceful teardown when explicit unload is requested
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
