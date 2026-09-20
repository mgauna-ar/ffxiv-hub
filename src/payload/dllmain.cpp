#include "payload/hook_manager.hpp"
#include "payload/dx11_hook.hpp"
#include "payload/overlay_host.hpp"
#include "payload/object_reader.hpp"
#include "common/ipc/ring_buffer.hpp"
#include "common/ipc/pipe_client.hpp"
#include "meter/combat_plugin.hpp"
#include "meter/combat_overlay.hpp"
#include "mitigator/latency_plugin.hpp"
#include "mitigator/latency_overlay.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#include <memory>

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
        return 0;
    }

    // 2. Initialize plugins and overlays
    auto latency_plugin = std::make_shared<hub::mitigator::LatencyPlugin>();
    latency_plugin->initialize();

    auto combat_plugin = std::make_shared<hub::meter::CombatPlugin>();
    combat_plugin->initialize();

    auto latency_overlay = std::make_shared<hub::mitigator::LatencyOverlay>();
    auto combat_overlay = std::make_shared<hub::meter::CombatOverlay>(&combat_plugin->engine());

    hub::payload::OverlayHost::instance().register_overlay(latency_overlay);
    hub::payload::OverlayHost::instance().register_overlay(combat_overlay);

    // 3. Initialize wait-free SPSC ring buffer for telemetry
    auto ring_buffer = std::make_shared<hub::payload::HookManager::RingBuffer>();

    // 4. Initialize HookManager and attach consumers
    auto& hook_mgr = hub::payload::HookManager::instance();
    hook_mgr.set_latency_consumer(latency_plugin.get());
    hook_mgr.set_meter_consumer(combat_plugin.get());
    hook_mgr.set_ring_buffer(ring_buffer.get());

    hook_mgr.install();

    // 5. Initialize ObjectReader
    auto object_reader = std::make_unique<hub::payload::ObjectReader>(ring_buffer.get());
    object_reader->initialize();

    // 6. Install DirectX 11 Hook (Present & ResizeBuffers)
    hub::payload::Dx11Hook::instance().install();

    // 7. Background orchestration loop
    auto last_party_sync = std::chrono::steady_clock::now();

    while (!g_shutdown_requested.load() && !hub::payload::Dx11Hook::is_shutting_down()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        auto now = std::chrono::steady_clock::now();

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
    }

    // Graceful teardown when explicit unload is requested
    hook_mgr.uninstall();
    hub::payload::Dx11Hook::instance().uninstall();

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
