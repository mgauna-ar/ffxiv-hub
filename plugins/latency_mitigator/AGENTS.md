# Latency Mitigator: Agent Guidelines

Plugin-local rules. The cross-cutting ones - single `ReceiveActionEffect` hook and its
phase ordering, IPC and ring buffer safety, hook lifecycle and teardown, tray and
dashboard decoupling - live in the root [`AGENTS.md`](../../AGENTS.md) and apply here too.

## Frozen Latency Mitigation Rules

Every rule below records a failure that was observed and fixed. Preserve them when
modifying detours or timing math.

- **No Client-Queued Action Skipping**: In FFXIV's client engine, `UseActionLocation` fires only once when the key is pressed. When an action is queued during the 0.5s GCD buffer window (`is_queued == true`), the game engine dequeues and transmits the packet via an internal engine routine without calling `UseActionLocation` a second time. Never skip dispatch recording or sample ingestion when `is_queued` is true or `animation_lock > 0`.
- **No Speculative Animation Lock Overwriting**: Never write speculative locks (e.g. 640ms) upon action dispatch. Server rejection leaves the player frozen.
- **No Bidirectional Outlier Tracking / Downward Reseeding**: TCP ACK coalescing delivers clusters of fast packets (15–25ms). Resetting the tracker triggers false cold-start states and intermittent mitigation dropouts.
- **Two-Tier Sequence Matching ($N$ vs $N+1$)**: Strategy 1 (exact sequence counter match) fails for queued actions because `UseActionLocation` fires before the sequence counter increments. Strategy 2 (oldest FIFO pending request matching `action_id`) must remain enabled for all actions.
- **Target Ping Definition**: `target_ping_ms` (default 15.0ms) represents simulated LAN ping near the datacenter. Never increase it to 40ms to absorb server frame ticks.
- **Caster Tax Preservation**: The 100ms lock after cast completion must remain unmitigated to prevent slide-cast clipping. It has two independent guards: the request's `is_cast` flag, and `CastTracker`, fed from `UseActionLocation` with the cast's *remaining* time whenever an action goes out mid-cast. Keep both fed.
- **Hard Anti-Cheat Clamping Floors**: Never allow `adjusted_lock` to drop below `min_animation_lock_ms` (25.0ms). A lock already at or under the floor is left untouched and is not a floor clamp.
- **Ceiling Is A Pass-Through, Not A Clamp**: Locks above `max_animation_lock_ms` (2500.0ms) are not mitigated at all (`clamped_by_ceiling`). Never clamp one down: a Limit Break's lock is legitimately longer, and ending it early lets the client act before the server allows.
- **Moving-Median Spike Filter**: Once 5 samples exist, a sample above `median + max(50ms, 0.5 × median, spike_multiplier × jitter)` (median over the `rtt_sample_window`, default 10) is replaced by the median in the EMA. The raw sample still enters the median window, so a sustained rise moves the median within about half a window; ingesting the median there instead froze the tracker at the old latency for good.
- **Config Is Sanitised In One Place**: `AnimationLockMitigator::set_config` (and every setter through it) clamps target ping and safety margin to $\ge 0$, `spike_multiplier` to $\ge 1$, the window to 1–64 and `max` to $\ge$ `min`. A negative ping or margin would cut the lock by more than the measured RTT.
- **Cold-Start Early Queue Guard**: Protect the first 4 samples from spike rejection to avoid poisoning the baseline on opening countdown queues.

