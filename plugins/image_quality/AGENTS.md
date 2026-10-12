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
- **Hand Back What Was Taken**: `Enforcer` records each field's game value before the plugin's first write to it and writes it back when the plugin lets go: master switch off, app disconnected, upscaler no longer DLSS, the field's own setting off, or `release()` on an unload. Teardown calls `release()` because no tick runs after it; the game's exit needs nothing. The current render height is never restored: the controller, once running again, moves it itself.
- **Pin Only A Settled Target**: The render thread rewrites the target and the current height together on a scale change (`0x1402db890`). The plugin pins the current height only to a target that read the same on the tick before, and never to one above the display height, so a race costs at most one tick at a height the controller could also have chosen.
- **The Orchestration Thread Owns It**: `update()`, the commands and the autosave all run on the payload's orchestration thread, and nothing else touches the plugin. It consumes no hook and pushes from no new thread. Each write is one aligned 1, 2 or 4 byte store, which the render thread reads whole.
- **Its Keys Live In `iq_settings.cpp`**: the table there is the only list of the `image_quality` section's keys and their defaults (`ImageQualitySettings{}`). `dlss_jitter_multiplier` is clamped to `MIN_JITTER_MULTIPLIER`-`MAX_JITTER_MULTIPLIER` on read and on every command. See the root AGENTS.md, *Every Plugin Key Has A Default On File*.
- **The App Hears The Game, Not The Settings**: `ImageQualityStatePayload` reports what the two singletons read after the tick's writes, sent on a change and every `STATE_KEEPALIVE_SECONDS`, nothing while switched off or disconnected. The view takes a report older than `AppState::kImageQualityStateTtl` as none. `found == 0` means a signature or a singleton is missing, and the plugin writes nothing then.

## How the client jitters and scales with DLSS

Read from `ffxiv_dx11.exe` on 2026-10-11 with `tools/inspect_exe.py`. Addresses are for
that build only; re-check after a patch.

- **`GraphicsConfig` is the render settings singleton.** `GRAPHICS_CONFIG_INSTANCE`
  resolves the global `0x1428f6470`, which holds a pointer to a `0xD8`-byte object built
  by `0x14025e020` in the device setup (`0x1400cf2f0`, the same one-time setup as the swap
  chain). The constructor stores `0.6f` at `+0x78` and `1` at `+0x45`, `+0x46` and `+0x47`.
  `+0x54` is the upscaler option plus one (`0x14025e573`, `0x1400ce179`): 1 FSR, 2 DLSS.
- **The jitter is computed in one method of the render camera** (`0x14025f870`, which
  writes the jittered projection to `Camera+0x50`). It jitters only if `+0x54 == 2` or the
  edge smoothing option `+0x2c == 2` (TSCMAA + camera jitter), and the camera's `+0x206`
  is set, its `+0x202` clear, its `+0x1e0` not `0x55`, and `GraphicsConfig+0x1a` clear.
  The offset in render pixels is a Halton(2,3) sample times
  `(jitter_multiplier - 0.001) * s^2 + 0.001`, where
  `s = (RTM.u16+0x700 / RTM.u16+0x704) * GraphicsConfig+0x4c`. `+0x700` is the display
  height and `+0x704` the display height times the scale, so `s` is 1 and the amplitude is
  `jitter_multiplier`. The camera keeps only that scaled offset (`+0x280`, the previous
  frame's at `+0x288`), so DLSS is handed the offset actually rendered. Under DLSS the
  Halton period is unbounded; otherwise it is `8 * (display / render)^2`.
- **`GraphicsConfig+0x1a` belongs to the lobby, not to cutscenes.** Its only writers are
  `0x1404d1170` (set) and `0x1404d11e0` (clear), reached through `AgentLobby` (agent 0
  from `GetAgentByInternalId`), plus two unreferenced copies (`0x1404c1000`,
  `0x1404c1030`). Whether a cutscene or a dialogue clears the camera's `+0x206` was not
  traced; its 38 writers span several classes.
- **DLSS always runs the resolution controller.** The controller (`0x1402da790`, the
  stepper at `0x1402dab17`) runs when `+0x54 == 2 || +0x44` (the checkbox), and `+0x45`
  is set. It accumulates the frame time and, each time that passes 0.5 s, moves
  `RTM+0x702` by 9 px (18 above 1440) between `RTM+0x706` (two thirds of the target) and
  `RTM+0x704` (the target). Under DLSS it steps down when the GPU time (`RTM+0x70c`, ms,
  from timestamp queries) exceeds `0.9 * max(RTM+0x710, budget)` and up below `0.8 * ...`;
  FSR uses 0.97 and 0.9. `RTM+0x710` is the smoothed frame time in ms
  (`old * 0.9833 + frame / 60`). The stepping is skipped when `GraphicsConfig+0x47` is 0;
  no other reader or writer of `+0x47` turned up among the global's 327 users. `0x1402db890` sets `+0x704`, `+0x706` and,
  on a change of target, `+0x702`.
- **`RenderTargetManager`** is the global `0x1428f6490`, holding a pointer.
  `RENDER_TARGET_MANAGER_INSTANCE` is FFXIVClientStructs' pattern extended by one
  instruction, which it needs to match once.
