# ⚔️ Combat Meter

Real-time damage and healing analytics: DPS, HPS with overheal separated out, crit and
direct hit rates, per-action breakdowns, and a pull history.

Part of [FFXIV Hub](../../README.md). Enable or disable it from its page in the desktop
app, or from its card on the dashboard.

---

## How encounters are tracked

### Starting and ending a pull

An encounter starts on the first direct action that deals damage. Blocked and parried
hits count, since those are mitigated rather than avoided.

Healing, shields, buffs, debuffs and whiffs never start one. Prepull topping-off and
prepotting is preparation, not combat, and opening a pull on it meant the clock was
already running — and the healer already ranked — before anyone had touched the boss.
Damage-over-time and heal-over-time ticks never start one either, or a lingering DoT on a
mob you walked away from would open a pull on its own.

Once the pull is underway, healing counts as normal, both toward HPS and as activity that
holds off the inactivity timeout.

It ends in one of four ways:

1. **Inactivity** — 7 seconds with no combat activity splits the encounter and archives it.
2. **Wipe** — every synced party member confirmed dead. One survivor, or a raise, cancels it.
   Party HP is read from the party list on each sync (every 1.5 s); a death or a raise is
   republished to the desktop app so both sides see the wipe. A member whose HP has never
   been read is not counted as dead.
3. **Zone change** — any in-progress pull is finalised and archived. The zone is read from
   the party list, so solo play has no zone and pulls are filed under *Unknown zone*.
   Going solo mid-pull (the party disbanding) is not a zone change: the pull carries on
   and keeps the zone it started in.
4. **Manual** — ended from the desktop app.

Duration is measured to the *last combat action*, not to the moment the timeout fired, so
a 7-second gap does not inflate the pull and deflate everyone's DPS. Wipes and zone
changes are trimmed the same way, since both are also detected after the fact. Only a
manual end takes the full elapsed time.

### Pet attribution

Pet damage belongs to the owner. When a pet acts — Demi-Bahamut's Akh Morn, Automaton
Queen's Pile Bunker, Living Shadow's Shadowbringer — the registry maps its entity id back
to the player and merges the stats there:

- Unlinked pets are attributed by matching party and local player jobs.
- Late attribution packets consolidate into the owner rather than leaving a stray row.
- A pet's link to its owner ends when its entity id comes back as something else, so a
  monster reusing that id is never merged into the old owner.
- Pet damage counts toward the player's total and DPS.
- It is also tracked separately, so you can see how much of a total came from the pet.
  A pet attributed late counts once toward that figure, and if the owner had no row yet,
  the merged row takes the owner's name and job rather than the pet's.
- Pet skills appear in the player's own per-action breakdown.

The result is no orphan rows: a raid table shows eight players, not eight players and
six pets.

### Healing and overheal

HPS is effective healing only. Overhealing is tracked and shown, but never counted toward
HPS — a healer topping off full-health party members should not out-rank one whose healing
landed. Total healing is effective plus overheal, and the overheal percentage is measured
against that total.

Overheal is worked out in-game, as each heal is decoded: the heal is compared against how
much HP its target was missing, read from the game at that moment. The game applies the HP
change in a later packet, so that reading is still the pre-heal value. The split is made
before the packet goes to the desktop app, so both views agree. A self-heal carried on an
attack (a drain) is measured against the caster, not the enemy. Two heals landing on the
same target before the game applies either one both see the same missing HP, so overheal
is slightly under-counted in that case.

### Limit Break

The game reports the casting player as the source of a Limit Break, so attributing it by
source would hand one player a large chunk of the raid's damage. It is identified by
action id instead and routed to its own synthetic row. It counts toward raid DPS, and
never toward any individual's damage, DPS or share.

### Other behaviour worth knowing

- **Blocked and parried hits are still damage.** They carry a damage value and are counted
  as hits; the block and parry counters are extra, not a replacement.
- **Ticks have no severity.** DoT and HoT ticks carry no crit flag from the game, so they
  are counted separately and kept out of the denominator for crit, DH and CDH rates.
  Including them would dilute every rate toward zero.
- **Enemy damage to players** is tracked as damage taken on the target, and never added to
  raid DPS. Each hit of an AoE is booked on the target it actually hit, so a self-centred
  AoE never lands on its caster and a raidwide never lands on the boss.
- **Crit and direct hit** come from the game's severity bits (`0x20` and `0x40`) only.

---

## Views

**In-game overlay.** A draggable table showing the current encounter. It displays one
metric at a time — damage or healing — selected by `overlay_metric`. Rows carry the
combatant's job colour, taken from the hub's shared job style table so the overlay and the
desktop always agree.

**Desktop view.** Three tabs: **Damage**, **Healing**, and **Settings**. Damage and
healing show a pull list rail on the left - the live fight at the top, then archived
pulls grouped by duty with their end time, duration and a clear/wipe badge - and the
selected pull's rankings beside it: share, crit, direct hit and crit-direct-hit rates with
job-coloured bars, plus a per-action drilldown with min/avg/max hits and swing counts. The
rail is the only place a pull is chosen; it narrows to pull numbers on a small window.

Job colours are per-job, not per-role, and live in `src/common/ui/job_style.cpp` as the
single source of truth.

---

## Configuration

Stored in the `combat_meter` section of `%APPDATA%/ffxiv-hub/config.json`. Everything here
is editable from the plugin's page in the desktop app; the file is the persistence format,
not the intended interface.

| Key | Type | Default | Description |
|---|---|---|---|
| `plugin_enabled` | bool | `true` | Master switch. Off means no hook dispatch, no telemetry, no overlay. |
| `inactivity_timeout_seconds` | float | `7.0` | Gap that splits one encounter from the next. |
| `party_only` | bool | `true` | In-game overlay only: restrict rows to the synced party and the Limit Break row. Solo there is no party list, so it keeps every friendly row. |
| `show_bars` | bool | `true` | Job-coloured progress bars behind rows. |
| `hide_inactive` | bool | `false` | Hide combatants with no activity. |
| `refresh_interval_ms` | int | `500` | How often the displayed snapshot refreshes. |
| `show_col_share` | bool | — | Show the damage share column. |
| `show_col_crit` | bool | — | Show the crit rate column. |
| `show_col_dh` | bool | — | Show the direct hit column. |
| `show_col_cdh` | bool | — | Show the crit-direct-hit column. |
| `overlay_metric` | int | `0` | In-game overlay metric: `0` damage, `1` healing. |
| `overlay_visible` | bool | `true` | Draw the in-game overlay. |
| `overlay_x`, `overlay_y` | float | `-1.0` | Position. Negative means never placed — the overlay picks its own default. |
| `overlay_width`, `overlay_height` | float | `800.0`, `480.0` | Size. |
| `overlay_opacity` | float | `0.88` | Background transparency. |
| `overlay_scale` | float | `1.0` | Font and table scaling. |
| `overlay_locked` | bool | `false` | Prevent dragging the overlay. |
| `overlay_click_through` | bool | `false` | Pass mouse clicks through to the game. |
| `overlay_hide_conditions` | int | `0` | Bitmask of game states that hide the overlay. Zero is always visible. |

An older `enabled` key is still read, so a configuration written before the master switch
existed keeps its meaning.

---

Engineering constraints for this plugin are in [`AGENTS.md`](AGENTS.md).
