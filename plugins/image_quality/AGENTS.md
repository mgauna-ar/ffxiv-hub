# Image Quality: Agent Guidelines

Plugin-local rules. The cross-cutting ones - IPC and ring buffer safety, hook lifecycle
and teardown, the master switch, config ownership - live in the root
[`AGENTS.md`](../../AGENTS.md) and apply here too.

## Rules

Rules about what the game does rest on
[How the client jitters and scales with DLSS](#how-the-client-jitters-and-scales-with-dlss),
which was verified against the binary. Check a change against that section, not against
a guess about the game.

- **Three Fields, Nothing Else**: The plugin writes `GraphicsConfig::dynamic_rezo_stepping` (`+0x47`), `GraphicsConfig::jitter_multiplier` (`+0x78`) and `RenderTargetManager`'s current render height (`+0x702`), and only through `hub::os::safe_copy`. A new write needs its own verified model in the section below first.
- **Only Under DLSS**: Every write is gated on `upscale_type == UpscaleType::Dlss`. FSR, including TSCMAA + camera jitter, keeps every game value. Never key a write on `dynamic_rezo_enable`: the client skips that checkbox under DLSS, so it says nothing about what the controller does.
- **The Target Is Not The Mode**: Under DLSS, `RTM+0x704` is the top of DLSS's own range (the screen height at 4K), not a quality mode, so pinning to it is DLAA. The pinned height is `pinned_height()` in `iq/render_math.hpp`: the screen height times `dlss_render_scale`, clamped to `[floor, target]`, the range the controller itself moves in.
- **The Screen Height Is Derived**: `RTM+0x700` is the height frames render at, not the screen's; taking it for the screen's pinned nothing once the controller had lowered the height. The screen height is `screen_height()`: the target over `RTM+0x71c`, the target's share of the screen. With that share unknown (0, or above 1) nothing is pinned.
- **Pin Only A Settled Range**: The render thread rewrites the range and the current height together when DLSS's range changes (`0x1402db890`). The plugin pins only inside a range that read the same on the tick before, and only one whose target does not pass the screen height, so a race costs at most one tick at a height the controller could also have chosen.
- **The Jitter Amount Is What The Client Applies**: The client scales the multiplier by `s^2` (see below), so the plugin writes `multiplier_for(amount, s^2)`, kept within `MIN_WRITTEN_MULTIPLIER`-`MAX_WRITTEN_MULTIPLIER`, and the view shows `effective_jitter()`. Both live in `iq/render_math.hpp`; never write `dlss_jitter_amount` as the multiplier. `s` follows the height frames render at, so the multiplier is rewritten on the tick after the height moves. With the heights unknown nothing is written.
- **Hand Back What Was Taken**: `Enforcer` records each field's game value before the plugin's first write to it and writes it back when the plugin lets go: master switch off, app disconnected, upscaler no longer DLSS, the field's own setting off, or `release()` on an unload. Teardown calls `release()` because no tick runs after it; the game's exit needs nothing. The current render height is never restored: the controller, once running again, moves it itself.
- **The Orchestration Thread Owns It**: `update()`, the commands and the autosave all run on the payload's orchestration thread, and nothing else touches the plugin. It consumes no hook and pushes from no new thread. Each write is one aligned 1, 2 or 4 byte store, which the render thread reads whole.
- **Its Keys Live In `iq_settings.cpp`**: the table there is the only list of the `image_quality` section's keys and their defaults (`ImageQualitySettings{}`). `dlss_render_scale` and `dlss_jitter_amount` are clamped on read and on every command. See the root AGENTS.md, *Every Plugin Key Has A Default On File*.
- **The App Hears The Game, Not The Settings**: `ImageQualityStatePayload` reports what the two singletons read after the tick's writes, sent on a change and every `STATE_KEEPALIVE_SECONDS`, nothing while switched off or disconnected. The view takes a report older than `AppState::kImageQualityStateTtl` as none. `found == 0` means a signature or a singleton is missing, and the plugin writes nothing then.

## How the client jitters and scales with DLSS

Read from `ffxiv_dx11.exe` on 2026-10-11 and 2026-10-12 with `tools/inspect_exe.py`.
Addresses are for that build only; re-check after a patch.

- **`GraphicsConfig` is the render settings singleton.** `GRAPHICS_CONFIG_INSTANCE`
  resolves the global `0x1428f6470`, which holds a pointer to a `0xD8`-byte object built
  by `0x14025e020` in the device setup (`0x1400cf2f0`, the same one-time setup as the swap
  chain). The constructor stores `0.6f` at `+0x78` and `1` at `+0x45`, `+0x46` and `+0x47`.
  `+0x54` is the upscaler option plus one (`0x14025e573`, `0x1400ce179`): 1 FSR, 2 DLSS.
  `+0x4c` is the 3D resolution slider over 100 (`0x1400ce150`).
- **The jitter is computed in one method of the render camera** (`0x14025f870`, which
  writes the jittered projection to `Camera+0x50`). It jitters only if `+0x54 == 2` or the
  edge smoothing option `+0x2c == 2` (TSCMAA + camera jitter), and the camera's `+0x206`
  is set, its `+0x202` clear, its `+0x1e0` not `0x55`, and `GraphicsConfig+0x1a` clear.
  The NDC offset is `2 * a * sample / RTM.Resolution` (`+0x428`, the render size: see
  below) for a Halton(2,3) sample, so `a` is in render pixels, with
  `a = (jitter_multiplier - 0.001) * s^2 + 0.001` and
  `s = RTM.u16+0x700 / RTM.u16+0x704 * GraphicsConfig+0x4c`: the height frames render at
  over the top of the range, times the slider. Under DLSS, `+0x704` is the top of DLSS's
  range, so the game's jitter shrinks as its controller lowers the height: at 4K and a
  77% slider, 0.36 at 2160, 0.16 at 1440 (seen live on 2026-10-12) and 0.09 at 1080. The
  camera keeps only that scaled offset (`+0x280`,
  the previous frame's at `+0x288`). Which value the DLSS pass hands NGX as its jitter
  (`0x1403757c0` reads it from its parameters at `+0x28`) was not traced back to the
  camera. Under DLSS the Halton period is unbounded; otherwise `8 * (screen / +0x428)^2`.
- **`RTM+0x700` is the height frames render at.** The controller's tail (from
  `0x1402dac36`), which runs whether or not `+0x47` let it step, calls `0x140236ca0`
  and copies `+0x702` into `+0x700` whenever the two differ (`+0x704` when the
  controller is off). The render target setup (`0x1402dc540`, through the thunk
  `0x1402dd230` the RTM registers with the device) writes it too (`0x1402dd1cd`). Only
  the RTM's first setup (`0x1402d5680`) sets it to the screen height, with `+0x702` and
  `+0x704`, which is how it once passed for the screen's.
- **`RTM+0x428` starts each range update as the screen size and ends as the render
  size.** Both callers of the range setter (`0x1402dafc0` and `0x1402dc540`) copy the
  device's output size (`Device+0x8c`, the global `0x1428efd00`) into it and pass it in;
  the setter (`0x1402db890`) rewrites it to the current height and its width to match
  (`0x1402dba38`, `0x1402dbb2e`).
- **`RTM+0x71c` is the target's share of the screen height.** The constructor
  (`0x1402d5280`) sets 1.0. Under DLSS the range setter stores NGX's largest render
  height over the screen's (`0x1402db928`); without it, the controller's tail stores the
  slider (`0x1402dace7`), and the target is the screen height times the slider. So
  `+0x704 / +0x71c` is the screen height either way.
- **`GraphicsConfig+0x1a` belongs to the lobby, not to cutscenes.** Its only writers are
  `0x1404d1170` (set) and `0x1404d11e0` (clear), reached through `AgentLobby` (agent 0
  from `GetAgentByInternalId`), plus two unreferenced copies (`0x1404c1000`,
  `0x1404c1030`). Whether a cutscene or a dialogue clears the camera's `+0x206` was not
  traced; its 38 writers span several classes.
- **DLSS always runs the resolution controller.** The controller (`0x1402da790`, the
  stepper at `0x1402dab17`) runs when `+0x54 == 2 || +0x44` (the checkbox), and `+0x45`
  is set. It accumulates the frame time and, each time that passes 0.5 s, moves
  `RTM+0x702` by 9 px (18 above 1440) between `RTM+0x706` (the floor) and `RTM+0x704`
  (the target). Under DLSS it steps down when the GPU time (`RTM+0x70c`, ms, from
  timestamp queries) exceeds `0.9 * max(RTM+0x710, budget)` and up below `0.8 * ...`;
  FSR uses 0.97 and 0.9. `RTM+0x710` is the smoothed frame time in ms
  (`old * 0.9833 + frame / 60`). The stepping is skipped when `GraphicsConfig+0x47` is 0
  (`0x1402daa2f`), but not the tail that applies a changed `+0x702`, which is what lets
  a pinned height take; no other reader or writer of `+0x47` turned up among the
  global's 327 users.
- **Under DLSS the range is DLSS's.** `0x1402db890` sets the target and the floor. When
  `RTM+0x730` holds a callback, it takes them from it: `0x1403524a0`, installed by
  `0x140352c40` when the device reports DLSS, answers only while `+0x54 == 2`, with the
  DLSS object's sizes for the selected mode (`0x1403746e0`): the target from `+0xe4` and
  the floor from `+0xdc`, NGX's largest and smallest render sizes. Seen live at 4K
  (2026-10-12, through the Live tab) with the DLSS mode forced by NVIDIA's driver
  override: 1080-2160 in Quality, 2138-2160 in DLAA, where the controller has one step
  left and the pin always lands near native.
  The mode's own render size (`+0xd4`, NGX's optimal one, or the screen times the slider
  when the object's `+0x180` is 1) is not used by the controller. Without the callback
  (FSR) the target is the screen height times the slider and the floor two thirds of it.
- **`GraphicsConfig+0x55` picks the DLSS mode** (`0x140374744`). Read in the order of
  NGX's `PerfQualityValue`, which the by-resolution case fits: 0 by resolution (up to
  1080p DLAA, 1440p Quality, 4K Performance, above Ultra Performance), 1 DLAA, 2 Ultra
  Quality, 3 Quality, 4 Balanced, 5 Performance, 6 Ultra Performance. Its only writers set
  it to 4 whenever the upscaler changes (`0x14025e590`, `0x1400ce19b`). Since the
  controller uses only the mode's range, the mode matters less than its name suggests.
- **`RenderTargetManager`** is the global `0x1428f6490`, holding a pointer.
  `RENDER_TARGET_MANAGER_INSTANCE` is FFXIVClientStructs' pattern extended by one
  instruction, which it needs to match once.
