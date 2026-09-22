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
- **Caster Tax Preservation**: The 100ms lock after cast completion must remain unmitigated to prevent slide-cast clipping.
- **Hard Anti-Cheat Clamping Floors**: Never allow `adjusted_lock` to drop below `min_animation_lock_ms` (25.0ms).
- **Corrupt Packet Ceiling**: Clamp incoming animation locks to `max_animation_lock_ms` (2500.0ms).
- **Moving-Median Spike Filter**: Reject RTT samples $> 2.5\times$ the moving median of the last 5 samples from the smoothed EMA.
- **Cold-Start Early Queue Guard**: Protect the first 4 samples from spike rejection to avoid poisoning the baseline on opening countdown queues.

