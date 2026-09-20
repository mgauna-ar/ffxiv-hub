# Agent Guidelines & Architecture Reference: FFXIV Hub

This document defines the architectural patterns, engineering principles, memory layouts, invariants, and verification workflows for autonomous AI agents and developers working on `ffxiv-hub`.

---

## 🏛️ Engineering Principles & Guidelines

1. **Strict Separation of Concerns (No God Files & Zero Duplication)**:
   - Every file must have a single, clearly defined responsibility.
   - Common infrastructure (memory scanning, PE section scanning, game signatures, IPC protocol multiplexing, JSON config, OS integration) lives strictly in `src/common/` (`hub::common`).
   - Distinct features are partitioned into modular plugins (`plugins/combat_meter/` and `plugins/latency_mitigator/`) implementing standard C++ interfaces (`IPlugin`, `IOverlay`, `IConfigurable`, `IHookConsumer`).
   - In-game hooking and DirectX 11 presentation reside in `src/payload/`.
   - The desktop GUI dashboard lives in `src/app/`.

2. **Clean Interfaces & 100% Platform-Independent Core**:
   - The algorithmic cores of all plugins (combat analytics, sequence tracking, RTT filters, animation lock formulas), along with the IPC protocol, config parser, and plugin interfaces, must remain **100% platform-independent C++20**, free of `<windows.h>` and DirectX headers.
   - OS-specific subsystems (`logger`, `single_instance`, `auto_start`, `process_finder`, `injector`, `tray_manager`) provide seamless `#ifdef _WIN32` cross-platform mock fallbacks.
   - This ensures the entire core analytics, timing math, and comprehensive unit test suite can be compiled and verified natively on macOS and Linux using `clang++` (`make test`) without emulator overhead.

3. **Memory Safety & Modern C++ Standards**:
   - Standard: C++20 (`-std=c++20` / `/std:c++20`).
   - Use RAII for all resource lifecycles (handles, synchronization primitives, pipes, DirectX COM interfaces).
   - Use `std::span` and typed binary structs for packet parsing rather than raw unbounded pointer arithmetic.
   - Enforce exact binary packing using `#pragma pack(push, 1)` and compile-time `static_assert(sizeof(...) == N)`.

4. **Zero-Dependency Runtime Constraint for Production Windows Binaries**:
   - The project builds a standalone desktop manager (`ffxiv-hub.exe`, built with `/SUBSYSTEM:WINDOWS`) and a single unified in-game payload DLL (`hub_payload.dll`) placed in the same directory, running on Windows 10/11 without requiring:
     - External plugin injectors (Dalamud, XIVLauncher).
     - External web servers or Node/Python runtimes.
     - Kernel filter drivers (WinDivert).
     - Microsoft Visual C++ Redistributable packages (statically linked `/MT` runtime).

5. **Unified Single-Hook Payload Lifecycle**:
   - Rather than injecting multiple independent DLLs into `ffxiv_dx11.exe`, `ffxiv-hub` injects **one single payload** (`hub_payload.dll`).
   - Exactly **one DirectX 11 hook** (`IDXGISwapChain::Present`, `ResizeBuffers`).
   - Exactly **one `WndProc` hook** for transparent mouse/keyboard input management.
   - Exactly **one multiplexed Named Pipe connection** (`\\.\pipe\ffxiv_hub_pipe`).
   - Exactly **one detour for `ReceiveActionEffect`**, dispatching sequentially and deterministically to `latency_mitigator` first, then to `combat_meter`.

6. **Mandatory Documentation Synchronization (`README.md` & `AGENTS.md`)**:
   - Whenever code is modified, the author or agent **must verify and update both `README.md` and `AGENTS.md`** if the change impacts:
     - Component architecture, translation units, or headers (`Component Responsibility Matrix`).
     - Binary packet structures or sizes (`Multiplexed Binary IPC Protocol`).
     - Critical invariants, timing thresholds, or anti-cheat guardrails.
     - Build commands, dependencies, or test instructions.
   - **Never leave documentation out of sync with code**. Both files must co-evolve with every pull request and agent task.

---

## 📂 Component Responsibility Matrix

| Component | Header / Source | Primary Responsibility |
|---|---|---|
| **Hub Types** | `include/hub/types.hpp` | Common enums (`PluginId`, `MessageType`, `CommandId`), rect/point geometry structs, error codes |
| **Plugin API** | `include/hub/plugin_api.hpp` | Abstract interfaces: `IPlugin`, `IOverlay`, `IConfigurable`, `IHookConsumer`, and `PluginRegistry` |
| **Game Definitions** | `include/hub/game_definitions.hpp` | Centralized FFXIV Dawntrail 7.x AOB signatures, `ActionManager` offsets, `CharacterObject` offsets, packet layouts |
| **Sigscan Engine** | `src/common/sigscan.hpp`<br>`src/common/sigscan.cpp` | IDA-style AOB pattern scanning, wildcard handling, PE section bounds matching, RIP-relative resolution |
| **PE Scanner** | `src/common/pe_scanner.hpp`<br>`src/common/pe_scanner.cpp` | SEH-protected Win32 PE section scanning (`scan_module_section`) for game process module searching |
| **IPC Protocol Multiplexer** | `src/common/ipc/protocol.hpp`<br>`src/common/ipc/protocol.cpp` | Fixed-size binary packet framing with `PluginId` header, typed packet structs, bounds-checked serialization |
| **Wait-Free Ring Buffer** | `src/common/ipc/ring_buffer.hpp` | Lock-free Single-Producer Single-Consumer (SPSC) ring buffer (4096 capacity) with cacheline-isolated atomic indices |
| **Payload IPC Client** | `src/common/ipc/pipe_client.hpp`<br>`src/common/ipc/pipe_client.cpp` | In-game Named Pipe client thread streaming multiplexed telemetry to the desktop manager |
| **Manager IPC Server** | `src/common/ipc/pipe_server.hpp`<br>`src/common/ipc/pipe_server.cpp` | Desktop Named Pipe server (`\\.\pipe\ffxiv_hub_pipe`), worker thread dispatching incoming packets to plugins |
| **JSON Parser / Serializer** | `src/common/config/json.hpp`<br>`src/common/config/json.cpp` | Zero-dependency, lightweight JSON value tree parser, serializer, and stringifier |
| **Config Manager** | `src/common/config/config_manager.hpp`<br>`src/common/config/config_manager.cpp` | Configuration persistence to `%APPDATA%/ffxiv-hub/config.json`, multi-plugin sections, screen boundary clamping |
| **File Logger** | `src/common/os/logger.hpp`<br>`src/common/os/logger.cpp` | Thread-safe diagnostic file logger (`%APPDATA%/ffxiv-hub/hub.log`) with session rotation to `hub.prev.log` |
| **Process Finder** | `src/common/os/process_finder.hpp`<br>`src/common/os/process_finder.cpp` | Win32 Toolhelp32 process snapshot scanning for `ffxiv_dx11.exe` and HWND window enumeration |
| **DLL Injector** | `src/common/os/injector.hpp`<br>`src/common/os/injector.cpp` | Injects `hub_payload.dll` into the game process via `CreateRemoteThread` + `LoadLibraryW` |
| **Single Instance Guard** | `src/common/os/single_instance.hpp`<br>`src/common/os/single_instance.cpp` | Named Win32 mutex (`Local\FFXIVHubSingleInstanceMutex`) and registered window wake-up message |
| **Auto-Start Registry** | `src/common/os/auto_start.hpp`<br>`src/common/os/auto_start.cpp` | Windows logon auto-start registration (`HKCU\Software\Microsoft\Windows\CurrentVersion\Run`) |
| **System Tray Manager** | `src/common/os/tray_manager.hpp`<br>`src/common/os/tray_manager.cpp` | Shell NotifyIcon, connection tooltip, native notifications, and 100% plugin-agnostic lifecycle context menu |
| **Latency Mitigator Plugin** | `plugins/latency_mitigator/` | Algorithmic RTT tracking (EMA + median spike filter), sequence matching, cast tracking, animation lock mitigation |
| **Combat Meter Types** | `plugins/combat_meter/include/meter/types.hpp` | Dawntrail 7.x jobs, roles, hit severities, effect types, stats structs, and config |
| **Action Decoder** | `plugins/combat_meter/include/meter/action_decoder.hpp`<br>`plugins/combat_meter/src/action_decoder.cpp` | Pure binary decoder unpacking `ActionEffectHeader` and 8 `ActionEffectEntry` records into combat packets |
| **Combatant Registry** | `plugins/combat_meter/include/meter/combatant_registry.hpp`<br>`plugins/combat_meter/src/combatant_registry.cpp` | Actor metadata cache, role mapping, party sync, automatic pet attribution, wipe detection |
| **Metrics Accumulator** | `plugins/combat_meter/include/meter/metrics_accumulator.hpp`<br>`plugins/combat_meter/src/metrics_accumulator.cpp` | Real-time DPS, HPS (effective vs overheal), Crit%, DH%, CDH%, and per-action breakdowns |
| **Encounter Engine** | `plugins/combat_meter/include/meter/encounter_engine.hpp`<br>`plugins/combat_meter/src/encounter_engine.cpp` | Encounter state machine (start on action, 7.0s inactivity split, wipe detection, pull history) |
| **Combat Meter Plugin** | `plugins/combat_meter/include/meter/combat_plugin.hpp`<br>`plugins/combat_meter/src/combat_plugin.cpp` | `IPlugin`, `IConfigurable`, and `IHookConsumer` implementation dispatching game actions to engine |
| **Payload Hook Manager** | `src/payload/hook_manager.hpp`<br>`src/payload/hook_manager.cpp` | Central MinHook lifecycle, hooking `ReceiveActionEffect`, `UseActionLocation`, and dispatching to plugins |
| **DX11 Hook** | `src/payload/dx11_hook.hpp`<br>`src/payload/dx11_hook.cpp` | MinHook detours for `IDXGISwapChain::Present`, `ResizeBuffers`, and transparent shutdown passthrough |
| **WndProc Hook** | `src/payload/wndproc_hook.hpp`<br>`src/payload/wndproc_hook.cpp` | Non-destructive `SetWindowLongPtrW` window procedure detour with ImGui input capture routing |
| **Overlay Host** | `src/payload/overlay_host.hpp`<br>`src/payload/overlay_host.cpp` | In-game Dear ImGui render loop coordinating independent floating overlay windows for active plugins |
| **Object Reader** | `src/payload/object_reader.hpp`<br>`src/payload/object_reader.cpp` | SEH-protected reader for `CharacterObject` metadata, HP, jobs, and party synchronization |
| **Latency Overlay** | `plugins/latency_mitigator/include/mitigator/latency_overlay.hpp`<br>`plugins/latency_mitigator/src/latency_overlay.cpp` | `IOverlay` implementation for in-game micro ping HUD badge |
| **Combat Overlay** | `plugins/combat_meter/include/meter/combat_overlay.hpp`<br>`plugins/combat_meter/src/combat_overlay.cpp` | `IOverlay` implementation for in-game combat analytical table |
| **MinHook Library** | `src/third_party/minhook/` | Embedded lightweight x86/x64 in-memory detour hooking library |
| **Dear ImGui Library** | `src/third_party/imgui/` | Embedded immediate-mode graphical UI library with Win32 and DirectX 11 backends |
| **Payload DLL Entry** | `src/payload/dllmain.cpp` | Injected DLL lifecycle, background orchestration, and persistent resident state |
| **Desktop App State** | `src/app/app_state.hpp`<br>`src/app/app_state.cpp` | Desktop application state machine, game supervisor, plugin configuration store, and telemetry router |
| **Desktop Theme & UI** | `src/app/ui/theme.hpp`<br>`src/app/ui/sidebar.hpp` | Modern slate dark theme, TrueType font loading, job color tokens, and responsive navigation sidebar |
| **Dashboard View** | `src/app/ui/view_dashboard.hpp`<br>`src/app/ui/view_dashboard.cpp` | Plugin-agnostic system overview, game process monitor, IPC server card, dynamic loaded plugins table, log actions |
| **Combat Meter View** | `src/app/ui/view_combat.hpp`<br>`src/app/ui/view_combat.cpp` | Damage table with job-colored progress bars, healing stats, pull history, action drilldown, and overlay settings |
| **Latency Mitigator View** | `src/app/ui/view_latency.hpp`<br>`src/app/ui/view_latency.cpp` | Real-time RTT curve, jitter, server monitor card, rolling action feed, and HUD settings |
| **Settings View** | `src/app/ui/view_settings.hpp`<br>`src/app/ui/view_settings.cpp` | System preferences, Windows auto-start toggle, config directory management, and live log reader |
| **Desktop App Entry** | `src/app/main.cpp` | Windows GUI subsystem (`wWinMain` / `/SUBSYSTEM:WINDOWS`), ImGui DX11/Win32 desktop window, tray message pump |
| **Test Framework** | `tests/test_framework.hpp`<br>`tests/test_main.cpp` | Header-only cross-platform test runner executable runnable on macOS, Linux, and Windows |

---

## 🔒 Critical Invariants & Edge Cases

When modifying detours, hooks, or timing/analytics math, the following invariants **must** be preserved:

### 1. Single `ReceiveActionEffect` Hook & Deterministic Interception Order
- `ReceiveActionEffect` in `ffxiv_dx11.exe` is intercepted by **one single MinHook detour** in `hook_manager.cpp`.
- Sequential execution order inside the hook:
  1. **Latency Mitigator First**: Reads target animation lock, checks sequence, updates RTT tracker, and modifies `ActionManager::animation_lock` to remove latency.
  2. **Combat Meter Second**: Operates in **read-only mode**, decoding `ActionEffectHeader` and target entries to accumulate damage, healing, and encounter events.
- Never alter packet contents in either plugin.

### 2. Frozen Latency Mitigation Rules
- **No Client-Queued Action Skipping**: In FFXIV's client engine, `UseActionLocation` fires only once when the key is pressed. When an action is queued during the 0.5s GCD buffer window (`is_queued == true`), the game engine dequeues and transmits the packet via an internal engine routine without calling `UseActionLocation` a second time. Never skip dispatch recording or sample ingestion when `is_queued` is true or `animation_lock > 0`.
- **No Speculative Animation Lock Overwriting**: Never write speculative locks (e.g. 640ms) upon action dispatch. Server rejection leaves the player frozen.
- **No Bidirectional Outlier Tracking / Downward Reseeding**: TCP ACK coalescing delivers clusters of fast packets (15–25ms). Resetting the tracker triggers false cold-start states and intermittent mitigation dropouts.
- **Two-Tier Sequence Matching ($N$ vs $N+1$)**: Strategy 1 (exact sequence counter match) fails for queued actions because `UseActionLocation` fires before the sequence counter increments. Strategy 2 (oldest FIFO pending request matching `action_id`) must remain enabled for all actions.
- **Target Ping Definition**: `target_ping_ms` (default 15.0ms) represents simulated LAN ping near the datacenter. Never increase it to 40ms to absorb server frame ticks.
- **Caster Tax Preservation**: The 100ms lock after cast completion must remain unmitigated to prevent slide-cast clipping.
- **Hard Anti-Cheat Clamping Floors**: Never allow `adjusted_lock` to drop below `min_animation_lock_ms` (25.0ms).
- **Corrupt Packet Ceiling**: Clamp incoming animation locks to `max_animation_lock_ms` (2500.0ms).
- **Moving-Median Spike Filter**: Reject RTT samples $> 2.5\times$ the moving median of the last 5 samples from the smoothed EMA.
- **Cold-Start Early Queue Guard**: Protect the first 4 samples from spike rejection to avoid poisoning the baseline on opening countdown queues.

### 3. Combat Analytics Invariants
- **Automatic Pet Attribution (Zero Orphan Rows)**: `CombatantRegistry::resolve_owner` must map pet actions to the owner. Unlinked pets infer owners from party jobs. Stats merge cleanly with zero orphan rows. Cyclic ownership loop guard depth = 8.
- **Safe Duration Floor (Anti-Division-by-Zero)**: Combat duration must be clamped to `std::max(duration_seconds, 1.0)`.
- **Accurate Overheal Accounting**: HPS is strictly `effective_healing / duration`. Total healing is `effective_healing + overhealing`. Overheal percentage evaluates against total healing without division-by-zero when healing is zero.
- **Party Wipe State Invariance**: A wipe triggers only when all tracked synced party members are confirmed dead (`party_dead == m_party_members.size()`). A surviving player or revive cancels the wipe.
- **7.0s Inactivity Timeout & Duration Accuracy**: Encounters auto-split after 7.0 seconds without combat activity. Duration is calculated from the time of the last combat activity (`m_last_activity_time - m_start_time`), not inflated by the 7.0s timeout window.
- **Friendly Raid Damage Isolation**: Enemy incoming damage to players is tracked under `damage_taken` on the target. Enemy damage must never be added to `m_total_damage` or raid DPS.
- **Direct Action Encounter Initiation Only**: Passive DoT/HoT ticks must never initiate encounters when combat state is Idle, Wipe, or Complete.

### 4. IPC & Ring Buffer Safety
- **Multiplexed Packet Framing**: Every packet sent over Named Pipe `\\.\pipe\ffxiv_hub_pipe` begins with the fixed 16-byte `PacketHeader`:
  `magic (0x46465848 "FFXH") | plugin_id (uint16_t) | message_type (uint16_t) | sequence (uint32_t) | payload_size (uint32_t)`.
- **Wait-Free SPSC Ring Buffer**: The in-game detour thread pushes combat events to a lock-free SPSC ring buffer (capacity 4096). Detour threads must never wait on mutexes. A background consumer thread streams packets over Named Pipe.
- **Exact Binary Struct Packing**: All IPC structs use `#pragma pack(push, 1)` and are verified with `static_assert(sizeof(...) == N)`.

### 5. Hook Lifecycle, DirectX 11 & OS Teardown Safety
- **Persistent In-Game Payload**: The payload DLL remains resident in `ffxiv_dx11.exe` for the life of the game session once injected.
- **Seamless IPC Reconnection**: When the desktop app closes, the payload hides overlays and pauses mitigation (dormant state). When the app restarts, it connects within 1s without re-hooking.
- **Window Readiness Guard**: The injector verifies `FindWindowW(L"FFXIVGAME", nullptr)` exists before injecting to avoid transient pre-boot splash swapchains.
- **Non-Destructive Hook Passthrough**: `uninstall()` sets `g_shutting_down = true`, but **never** unhooks DXGI targets via `MH_DisableHook` or un-subclasses `WndProc` via `SetWindowLongPtrW`. Shutdown flags make hooks zero-overhead passthroughs forwarding directly to original function pointers.
- **Process Teardown Safety**: Background threads must never touch DirectX COM objects or call `MH_Uninitialize()` during OS process exit. `RtlDllShutdownInProgress()` is checked to detect termination.
- **MRT Pipeline Protection**: Render overlay functions must preserve and restore all 8 OM render target slots and the depth-stencil view.
- **SEH Memory Protection**: All game pointer dereferences must be guarded with `__try / __except` in leaf functions without local C++ objects requiring stack unwinding (avoiding MSVC C2712).

### 6. Decoupled Desktop Manager & Plugin-Agnostic Tray/Dashboard
- **Plugin-Agnostic System Tray**: The System Tray manager (`TrayManager`) must remain **100% decoupled and agnostic of specific plugins**. It must never contain plugin-specific actions, toggles, or metrics. Its responsibilities are strictly confined to the Hub application lifecycle: Show/Hide Main Window, Run at Startup, Open Logs/Config, and Exit.
- **Decoupled Dashboard View**: The overview Dashboard (`ViewDashboard`) must remain **100% decoupled and plugin-agnostic**. It must never display hardcoded plugin metrics (such as DPS, HPS, or ping cards). It renders only generic FFXIV process detection status, IPC Named Pipe server state, a dynamic loaded plugins list queried from the `PluginRegistry` metadata, and Hub utility actions. All plugin-specific metrics, graphs, tables, and overlay controls belong exclusively inside their respective views (`ViewCombat`, `ViewLatency`).

---

## 🎮 Game Structures & Memory Layouts (Dawntrail 7.x)

Located in [`include/hub/game_definitions.hpp`](include/hub/game_definitions.hpp):

### `CharacterObject` Offsets
- `0x30`: `char name[64]` (Player / NPC / Pet name)
- `0x78`: `uint32_t entity_id` (32-bit unique game entity ID)
- `0x88`: `uint32_t owner_id` (Pet master entity ID, 0 / 0xE0000000 if none)
- `0x90`: `uint8_t object_kind` (1=Player, 2=Monster, 3=NPC, 5=Pet)
- `0x1AC`: `uint32_t current_hp`
- `0x1B0`: `uint32_t max_hp`
- `0x1B4`: `uint32_t current_mp`
- `0x1B8`: `uint32_t max_mp`
- `0x1CA`: `uint8_t class_job` (Job ID e.g. 21=WAR, 41=VPR, 42=PCT)

### `ActionManager` Offsets
- `0x08`: `float animation_lock` (Active lock timer in seconds)
- `0x28`: `bool is_casting` (Active cast state)
- `0x30`: `float elapsed_cast_time`
- `0x34`: `float cast_time`
- `0x60`: `float remaining_combo_time`
- `0x68`: `bool is_queued`
- `0x120`: `uint16_t current_sequence` (Rolling action sequence ID)

### `ActionEffectHeader` Packet Layout (0x28 bytes)
- `0x00`: `uint64_t animation_target_id`
- `0x08`: `uint32_t action_id`
- `0x0C`: `uint32_t global_sequence`
- `0x10`: `float animation_lock`
- `0x14`: `uint32_t ballista_entity_id`
- `0x18`: `uint16_t source_sequence`
- `0x1A`: `uint16_t rotation`
- `0x1C`: `uint16_t spell_id`
- `0x1E`: `uint8_t animation_variation`
- `0x1F`: `uint8_t action_type`
- `0x20`: `uint8_t flags`
- `0x21`: `uint8_t num_targets`

---

## 🧪 Verification & Test Commands

### Running Unit Tests (macOS / Linux / Windows)
```bash
make test
```
Or directly with Clang:
```bash
clang++ -std=c++20 -Wall -Wextra -Wpedantic -Werror \
  -Iinclude -Isrc -Itests -Iplugins -Iplugins/latency_mitigator/include -Iplugins/combat_meter/include \
  src/common/*.cpp src/common/ipc/*.cpp src/common/config/*.cpp src/common/os/*.cpp \
  plugins/latency_mitigator/src/*.cpp plugins/combat_meter/src/*.cpp tests/*.cpp \
  -o hub_test_runner && ./hub_test_runner
```

### Windows MSVC Build
```cmd
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```
