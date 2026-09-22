# Latency Mitigator: Agent Guidelines

Plugin-local rules. The cross-cutting ones - single `ReceiveActionEffect` hook and its
phase ordering, IPC and ring buffer safety, hook lifecycle and teardown, tray and
dashboard decoupling - live in the root [`AGENTS.md`](../../AGENTS.md) and apply here too.

## Frozen Latency Mitigation Rules

Every rule below records a failure that was observed and fixed. Preserve them when
modifying detours or timing math. Rules about what the game does rest on
[How the client dispatches an action](#how-the-client-dispatches-an-action), which was
verified against the binary: check a change against that section, not against a guess
about the game.

- **No Client-Queued Action Skipping**: A `UseActionLocation` call that sees `is_queued == true` is the queued action *being sent*, not the key-press that queued it. Never skip, cap or discount its recording or its RTT sample because of `is_queued`, or because `animation_lock > 0` (every successful send leaves a provisional 0.5s lock). It is timestamped at the send, so its elapsed time is a true round trip with no queue wait in it.
- **No Speculative Animation Lock Overwriting**: Never write speculative locks (e.g. 640ms) upon action dispatch. Server rejection leaves the player frozen.
- **No Bidirectional Outlier Tracking / Downward Reseeding**: TCP ACK coalescing delivers clusters of fast packets (15–25ms). Resetting the tracker triggers false cold-start states and intermittent mitigation dropouts.
- **Two-Tier Sequence Matching**: Strategy 1 (exact sequence counter match) is the primary path for every send, queued ones included: the hooked `UseActionLocation` increments the counter before it sends, and the plugin reads it after the call returns, so the recorded sequence is the one on the wire. Strategy 2 (oldest FIFO pending request matching `action_id`) is the fallback for a response whose sequence finds no pending request, and must remain enabled for all actions. Never justify a change to either by "queued actions are recorded before the counter increments": they are not.
- **Target Ping Definition**: `target_ping_ms` (default 15.0ms) represents simulated LAN ping near the datacenter. Never increase it to 40ms to absorb server frame ticks.
- **Caster Tax Preservation**: The 100ms lock after cast completion must remain unmitigated to prevent slide-cast clipping. It has two independent guards: the request's `is_cast` flag, and `CastTracker`, fed from `UseActionLocation` with the cast's *remaining* time whenever an action goes out mid-cast. Keep both fed.
- **Hard Anti-Cheat Clamping Floors**: Never allow `adjusted_lock` to drop below `min_animation_lock_ms` (25.0ms). A lock already at or under the floor is left untouched and is not a floor clamp.
- **Ceiling Is A Pass-Through, Not A Clamp**: Locks above `max_animation_lock_ms` (2500.0ms) are not mitigated at all (`clamped_by_ceiling`). Never clamp one down: a Limit Break's lock is legitimately longer, and ending it early lets the client act before the server allows.
- **Moving-Median Spike Filter**: Once 5 samples exist, a sample above `median + max(50ms, 0.5 × median, spike_multiplier × jitter)` (median over the `rtt_sample_window`, default 10) is replaced by the median in the EMA. The raw sample still enters the median window, so a sustained rise moves the median within about half a window; ingesting the median there instead froze the tracker at the old latency for good.
- **Config Is Sanitised In One Place**: `AnimationLockMitigator::set_config` (and every setter through it) clamps target ping and safety margin to $\ge 0$, `spike_multiplier` to $\ge 1$, the window to 1–64 and `max` to $\ge$ `min`. A negative ping or margin would cut the lock by more than the measured RTT.
- **Cold-Start Guard**: While fewer than 5 samples exist there is no median to filter against, so samples are capped instead: the first at 200ms, the rest at `baseline + max(50ms, baseline / 2)`, flagged `cold_start_guard`. The median filter takes over after that. Queueing cannot inflate these samples, so do not attribute an inflated opener to it.

## How the client dispatches an action

Verified by disassembling `ffxiv_dx11.exe` (the Dawntrail 7.x client in `~/ffxiv/game`,
September 2026). Re-verify it after a game patch before changing any rule that depends on
it: `tools/check_signatures.py` confirms the functions are still where the signatures say,
and `objdump -d --x86-asm-syntax=intel` on macOS disassembles them. Offsets are
ActionManager fields from `include/hub/game_definitions.hpp`.

- **The hooked `UseActionLocation` is the send.** It returns 0 immediately while the animation lock (`+0x08`) is above 0. Otherwise it increments the sequence counter (`+0x120`, skipping 0 on wrap), sends, and writes a provisional client lock of 0.5s (0.35s for a few actions) to `+0x08`, which the server's value replaces in `ReceiveActionEffect`.
- **A key-press during a lock only queues.** `UseAction` stores the request in the queue fields (flag `+0x68`, type `+0x6C`, id `+0x70`, target `+0x78`) and returns. Nothing is sent, so the plugin records nothing at the key-press.
- **The queue fires from `ActionManager::Update`.** Once the lock reaches 0 it calls `UseAction` in queued mode, which calls the hooked `UseActionLocation`: that is the real send, and the plugin timestamps it then. `+0x68` is cleared only after that call returns, which is why `is_queued` reads true during the send.
- **Cast state.** A cast start writes the cast's action type to `+0x28` (a dword, which `ACTION_MANAGER_IS_CASTING` reads as a byte), its action id to `+0x2C`, resets elapsed time `+0x30` to 0, and writes the total cast time to `+0x34`. Whether `+0x28` is cleared at cast end was not traced.

