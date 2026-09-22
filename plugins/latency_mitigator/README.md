# ⚡ Latency Mitigator

Eliminates animation lock clipping so off-GCD abilities double-weave cleanly on medium to
high latency connections (50 ms – 300+ ms), without reducing lock to zero or allowing
action rates the server would reject.

Part of [FFXIV Hub](../../README.md). Enable or disable it from its page in the desktop
app, or from its card on the dashboard.

---

## How it works

### Two different latencies

The plugin measures two things that are easy to confuse:

- **Network ping** — wire latency, measured by a background ICMP prober against the game
  server IP, which is discovered from the client's own active TCP connections. It updates
  continuously, in or out of combat.
- **Action RTT** — the true combat round trip: the time from your client dispatching an
  action (`UseActionLocation`) until the server's effect packet comes back
  (`ReceiveActionEffect`). This is the number that governs animation lock.

Action RTT is always higher than ping, because the server has to evaluate combat logic and
cooldowns between the two events. That gap is server frame processing, not network
distance, and it is the part that makes weaving feel late.

### What gets adjusted

When the server's effect packet arrives it carries an animation lock value. The plugin
subtracts the measured round trip from that lock — it does not invent a new one — so the
lock you experience approximates what a player sitting next to the datacenter would feel.

The round trip feeding that subtraction is smoothed with an exponential moving average, so
a single slow packet does not swing the adjustment. On top of the EMA, a moving-median
filter rejects samples more than 2.5× the median of the recent window; TCP acknowledgement
coalescing delivers bursts of artificially fast packets, and without the filter those
would drag the baseline down and cause mitigation to cut in and out.

Matching a returning packet to the action that caused it uses two strategies. The exact
sequence counter match handles ordinary actions. Queued actions need the second one:
`UseActionLocation` fires when you press the key, before the sequence counter increments,
so those are matched against the oldest pending request with the same action id.

### What is deliberately left alone

- **Caster tax.** The 100 ms lock after a cast completes is never mitigated. Trimming it
  would clip slide-casting and desynchronise caster rotations from the server.
- **The floor.** Adjusted lock never drops below 25 ms, whatever the measured latency.
- **Corrupt values.** Incoming locks above 2500 ms are clamped rather than trusted.
- **Rejected actions.** If the server refuses an action, nothing is written.

The plugin never writes a speculative lock at dispatch time. Guessing before the server
answers and guessing wrong leaves the character frozen.

---

## In-game HUD

A small badge rendered directly into the game's backbuffer, showing network ping and
Action RTT. Drag it to reposition when unlocked; lock it to hold position; enable
click-through so clicks pass to the game.

**Display modes** (`overlay_mode`):

| Mode | Value | Shows |
|---|---|---|
| Compact Inline | `0` | Both metrics on one line |
| Two-Row Stacked | `1` | Ping and RTT on separate rows |
| Ping Only | `2` | Wire ping alone |

**Ping grading.** The badge is colour-graded against shared thresholds
(`PING_GRADE_GOOD_MS` / `FAIR_MS` / `POOR_MS` in
[`types.hpp`](include/mitigator/types.hpp)), and the desktop view uses the same bands so
the two never disagree:

| Band | Threshold | Meaning |
|---|---|---|
| Good | < 180 ms | Optimal |
| Fair | ≤ 260 ms | Reasonable cross-region latency |
| Poor | ≤ 340 ms | High latency |
| Bad | > 340 ms | Critical / route degradation |

A dimmed reading means no measurement yet — idle out of combat, or waiting on the first
server response.

---

## Configuration

Stored in the `latency_mitigator` section of `%APPDATA%/ffxiv-hub/config.json`. Everything
here is editable from the plugin's page in the desktop app; the file is the persistence
format, not the intended interface.

| Key | Type | Default | Description |
|---|---|---|---|
| `plugin_enabled` | bool | `true` | Master switch. Off means no hook dispatch, no telemetry, no overlay. |
| `enabled` | bool | `true` | Stops the animation-lock write-back only. Narrower than `plugin_enabled`: the plugin still measures and reports. |
| `dry_run` | bool | `false` | Calculate and stream telemetry without modifying game memory. |
| `target_ping_ms` | float | `15.0` | Simulated LAN latency near the datacenter. |
| `min_animation_lock_ms` | float | `25.0` | Hard floor. Adjusted lock never goes below this. |
| `max_animation_lock_ms` | float | `2500.0` | Ceiling clamp for malformed server packets. |
| `spike_multiplier` | float | `2.5` | Reject samples above this multiple of the moving median. |
| `overlay_visible` | bool | `true` | Draw the in-game HUD. |
| `overlay_mode` | int | `0` | Display layout — see the table above. |
| `overlay_x`, `overlay_y` | float | `20.0` | HUD position. |
| `overlay_width`, `overlay_height` | float | `120.0`, `32.0` | HUD size. |
| `overlay_opacity` | float | `0.90` | Background transparency. |
| `overlay_scale` | float | `1.0` | Font and badge scaling. |
| `overlay_locked` | bool | `false` | Prevent dragging the HUD. |
| `overlay_click_through` | bool | `false` | Pass mouse clicks through to the game. |
| `overlay_hide_conditions` | int | `0` | Bitmask of game states that hide the HUD. Zero is always visible. |

---

## FAQ

**Why does the HUD show a higher RTT than my ping test?**

They measure different things. Network ping is packet travel time. Action RTT includes the
server evaluating combat logic and cooldowns before it answers, which adds tens of
milliseconds on top of raw ping. The mitigator works from Action RTT, because that is what
actually delays your next weave.

**I have 200 ms latency — should I raise the target ping?**

No. The 15 ms default represents the ideal latency of a player near the datacenter, and it
is the target the engine subtracts *toward*. Leaving it alone is what grants full
double-weaving regardless of physical distance. Raising it mitigates less, not more.

**Is this safe?**

It enforces a hard 25 ms floor, never reduces lock to zero, preserves cast locks so
slide-casting is unaffected, and ignores actions the server rejected. It does not
manipulate packets or produce action frequencies the server would not otherwise permit.

---

Engineering constraints for this plugin are in [`AGENTS.md`](AGENTS.md).
