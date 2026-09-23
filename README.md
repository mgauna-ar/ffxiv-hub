# FFXIV Hub 🎮⚡

[![CI & Release](https://github.com/mgauna-ar/ffxiv-hub/actions/workflows/ci.yml/badge.svg)](https://github.com/mgauna-ar/ffxiv-hub/actions/workflows/ci.yml)
![Platform](https://img.shields.io/badge/platform-Windows%20x64-blue)
![C++20](https://img.shields.io/badge/standard-C%2B%2B20-crimson)
![License](https://img.shields.io/badge/license-MIT-green)

A combat meter and a latency mitigator for **Final Fantasy XIV (Dawntrail 7.x)**, in one
app with in-game overlays.

- **Combat Meter** — live DPS and HPS, who died and to what, buff and debuff uptime, and a
  history of every pull.
- **Latency Mitigator** — trims animation lock down to what a player next to the
  datacenter would feel, so oGCDs weave cleanly on a high-ping connection.

Two files, no installer. It does not need Dalamud, XIVLauncher, a Python runtime, or any
driver, and nothing else has to be installed for it to run.

---

## 🚀 Quick Start

### 1. Download & extract

Grab `ffxiv-hub-windows-x64.zip` from the [Releases](../../releases) tab and extract it
anywhere. Keep both files in the same folder — the app resolves the payload next to its
own executable:

```
ffxiv-hub/
├── ffxiv-hub.exe      # Desktop manager, system tray, injector
└── hub_payload.dll    # In-game hooks, overlays and telemetry
```

### 2. Run it

Double-click `ffxiv-hub.exe`. It opens the desktop manager and places an icon in the
notification area. There is no console window and nothing is installed.

> **Windows may warn you the first time.** The binaries are not code-signed. See
> [Troubleshooting](#-troubleshooting) below — this is expected, and the fix is two clicks.

### 3. Launch Final Fantasy XIV

Start the game through your normal launcher. The Hub discovers `ffxiv_dx11.exe`, injects
the payload once the game window is ready, and reports the attachment in the dashboard and
as a notification. Closing the game returns it to a waiting state; closing the Hub leaves
the game running and hides the overlays until you start it again.

> **There is nothing to configure.** The Latency Mitigator measures your own round-trip
> time and adapts on its own — you do not need to enter your ping or tune anything for it
> to work.

---

## 🧩 Plugins

Each plugin has a master switch — off means it consumes no game hooks, streams no
telemetry and draws no overlay, not merely that its view is hidden. Toggle it from the
plugin's page or its dashboard card.

| Plugin | What it does |
|---|---|
| [**Combat Meter**](plugins/combat_meter/README.md) | DPS and HPS with overheal separated, crit/DH/CDH rates, automatic pet attribution, deaths with killing blow and recap, buff/debuff/DoT uptime, damage taken by ability, encounter tracking and pull history |
| [**Latency Mitigator**](plugins/latency_mitigator/README.md) | Animation lock compensation for clean double-weaving on high latency, with a live ping/RTT HUD |

Each plugin's own README documents how it works, its in-game overlay, and its
configuration keys.

---

## 🖥️ What the app gives you

- **In-game overlays** for each plugin, drawn onto the game's own backbuffer. Drag them
  anywhere, scale them, set their opacity, lock them in place, or make them click-through.
  They respect variable refresh rate displays and coexist with ReShade and OBS.
- **A desktop window** with a sidebar: an overview dashboard, a page per plugin, and
  settings. Everything reflows as you resize it.
- **A system tray icon** with a live status tooltip. Show or hide the window, toggle
  starting with Windows, open the config folder or the log, and exit.
- **Settings that persist** — window and overlay positions, sizes, opacities and scales
  are all saved as you change them.

---

## ❓ Troubleshooting

**Windows Defender or SmartScreen flags the download.**

Expected, and safe to allow. The Hub loads its payload into the game using standard
Windows APIs (`VirtualAllocEx` and `CreateRemoteThread`) — the same mechanism a debugger
uses. Antivirus heuristics flag *any* unknown binary that does this unless it carries a
commercial Extended Validation code-signing certificate, which costs several hundred
dollars a year and is not something an open-source hobby project buys. Click
**More info → Run anyway**, or add an exclusion. The entire source is in this repository
and you can build it yourself.

**The overlays are not showing up.**

Check that the plugin's master switch is on (dashboard card or its page header), then that
the overlay itself is enabled in that plugin's settings. Overlays can also be set to hide
in specific situations — in a cutscene, out of combat, while a menu is open — so check the
hide conditions in the plugin's in-game overlay settings.

**How do I stop it?**

Exit from the tray menu. The game keeps running, untouched — the overlays disappear and
mitigation stops. The payload stays loaded in the game for that session by design, since
unloading code out from under a running DirectX pipeline is riskier than leaving it
dormant. It does nothing while the Hub is closed, and reconnects within a second if you
open the Hub again. Fully removing it is just closing the game.

To uninstall, delete the folder you extracted. Settings live in
`%APPDATA%/ffxiv-hub/` — delete that too if you want no trace left.

**Final Fantasy XIV just patched and it stopped working.**

A patch can move the memory locations the Hub relies on. Check the
[Releases](../../releases) page for an updated build. Nothing is damaged in the meantime:
if the Hub cannot find what it needs it reports the failure and does not attach.

**Does this risk my account?**

It is client-side only. It does not modify network packets, alter global cooldowns, or
automate anything — it adjusts a local timer after the server has already confirmed an
action, and enforces a floor so that timer never goes below what a player on a fast
connection would naturally have. The Combat Meter only reads.

---


---

## ⚙️ Configuration

Settings live in `%APPDATA%/ffxiv-hub/config.json`, written as you change them in the app.
The running game also saves live overlay state every few seconds, but only into the plugin
sections, merged into the file as it is on disk, so it never reverts a hub-level setting.
**Reset all settings** (Settings → Configuration) puts every key back to its default, in the
app and in-game, and switches every plugin back on. Start with Windows keeps following the
registry.
Each plugin owns a section; hub-level keys are:

| Key | Type | Default | Description |
|---|---|---|---|
| `start_with_windows` | bool | `false` | Launch automatically on Windows logon. |
| `minimize_to_tray` | bool | `true` | Closing the window hides to the notification area instead of exiting. |
| `show_notifications` | bool | `true` | Windows notifications on attach and detach. |
| `refresh_interval_ms` | int | `500` | How often the desktop views refresh. |

Plugin keys are documented in
[Combat Meter](plugins/combat_meter/README.md#configuration) and
[Latency Mitigator](plugins/latency_mitigator/README.md#configuration).

---

## 🛠️ Build & Development

### macOS & Linux (Unit Tests)
All core calculations, timing algorithms, IPC serialization, and configuration logic can be compiled and verified natively on macOS:
```bash
make test
```

### Windows (Full Production Build)
Requires Visual Studio 2022 (MSVC C++20) and CMake 3.20+:
```cmd
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```
Build outputs:
- `build/bin/Release/ffxiv-hub.exe`
- `build/bin/Release/hub_payload.dll`

### Packaging & Release Distribution
To generate the portable release ZIP package locally:
```cmd
cd build
cpack -G ZIP -C Release
```
Or via PowerShell:
```powershell
Compress-Archive -Path build/bin/Release/ffxiv-hub.exe, build/bin/Release/hub_payload.dll -DestinationPath ffxiv-hub-windows-x64.zip
```
The GitHub Actions CI/CD pipeline automatically compiles, tests, and publishes `ffxiv-hub-windows-x64.zip` with SHA256 checksums on all `v*.*.*` release tags and as workflow run artifacts on pushes to `main`.

---

## 📄 License & Fair Use

Bundled third-party assets:
- **Dear ImGui** (MIT) - vendored under `src/third_party/imgui/`.
- **MinHook** (BSD-2-Clause) - vendored under `src/third_party/minhook/`.
- **Lucide icons** (ISC) - a 50-glyph subset is embedded in `src/common/ui/icons_font.inl`;
  regenerate it with `tools/embed_icon_font.py`.

Final Fantasy XIV is a registered trademark of Square Enix Co., Ltd.
This project is an independent open-source utility designed for non-commercial educational and diagnostic purposes.
