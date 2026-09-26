# Combat Meter: Agent Guidelines

Plugin-local rules. The cross-cutting ones - single `ReceiveActionEffect` hook and its
phase ordering, IPC and ring buffer safety, hook lifecycle and teardown, tray and
dashboard decoupling - live in the root [`AGENTS.md`](../../AGENTS.md) and apply here too.

Two meter invariants deliberately stay in the root as well, because code outside this
directory can violate them: `EncounterEngine` is shared state reached from three threads,
and derived rates are recomputed on the tick rather than per packet.

## Combat Analytics Invariants

Every rule below records a failure that was observed and fixed, several of them against
captured log lines. Preserve them when modifying the registry, accumulator or engine.

- **Automatic Pet Attribution (Zero Orphan Rows)**: `CombatantRegistry::resolve_owner` must map pet actions to the owner. An actor is a pet when it has an owner, or when its `CharacterObject::object_kind` is `game::ObjectKind::Kind5`; that kind's meaning is unverified (see the root `AGENTS.md` game structures section) and the classification is kept as is until it is checked. Unlinked pets infer owners from party jobs. Stats merge cleanly with zero orphan rows. Cyclic ownership loop guard depth = 8. On a late merge the owner's `pet_damage` grows by the merged row's `total_damage` alone (its own `pet_damage` is already inside it), and a missing owner row is created from the registry, never by moving the pet's row across.
- **Effect Blocks Map To `targets[t]`**: Every effect block, slot 0 included, belongs to the matching entry of the target list; `animation_target_id` is only the fallback for a null list or empty slot. A self-centred AoE animates on its caster.
- **Safe Duration Floor (Anti-Division-by-Zero)**: Combat duration must be clamped to `std::max(duration_seconds, 1.0)`.
- **Elapsed Time Is Never Negative**: Every duration the engine derives from a caller's `now` against `m_start_time` goes through `elapsed_since_start_locked`, which clamps at 0. `current_summary`, `current_rankings` and `timeline` take `now` as a default argument, evaluated before `m_mutex` is taken, so the detour thread can start a pull in between; the unclamped difference came out negative, and `static_cast<uint64_t>(dur * 1e6)` on it is undefined (UBSan failed `MeterEngine.ConcurrentProducersAndReaders` on it). Such a `now` reads as no time elapsed: duration 0 and `end_time_us == start_time_us` (`MeterEngine.ReaderNowBeforeThePullStartReadsZero`).
- **Accurate Overheal Accounting**: HPS is strictly `effective_healing / duration`. Total healing is `effective_healing + overhealing`. Overheal percentage evaluates against total healing without division-by-zero when healing is zero.
- **Overheal Is Split Before Anything Records It**: The decoder emits a heal as all effective. `CombatPlugin` splits it with `decoder::apply_overheal` against the target's pre-heal HP from its `HpResolver` *before* `process_action` and the ring-buffer push, so the in-game and desktop engines hold identical numbers. A HoT tick is split the same way in `on_status_tick`: `damage_or_heal` stays the full tick and `CombatStatusTickPayload::overheal` carries the part that overhealed. Nothing downstream may re-split. A heal flagged on-source (`flags & 0x80`) targets the caster. The client applies an action's HP after its effect, which makes the pre-heal reading hold; a tick's HP comes in a separate packet whose order the server decides. See "How the client reads an effect entry" and "How the client reports DoT and HoT ticks".
- **Party Wipe State Invariance**: A wipe triggers only when all tracked synced party members are confirmed dead (`party_dead == m_party_members.size()`). A surviving player or revive cancels the wipe. *Confirmed dead* is `max_hp > 0 && current_hp == 0`: an unread HP proves nothing, and `register_actor` keeps a real 0 as 0. Solo, the party is the local player alone: with no synced party the check reads `m_local_player_id` and nothing else. Counting every player ever registered let a former party member, a stranger or a pet last read alive block every solo wipe. With no local id the old all-players count stands, pets excluded.
- **HP Must Keep Flowing**: Wipe detection is only as live as the HP it reads. `ObjectReader::sync_party` calls `update_hp` on every sync, and the actor cache's dedupe carries an alive/dead bit so a death or raise republishes `CombatActorInfo` to the app. Taking HP out of that path, or out of the dedupe entirely, silently turns every wipe into a combat end. Solo the party list is empty, so `sync_party` reads the local player's own object instead and publishes it through the same dedupe.
- **The Game's Combat Ends A Pull**: `EncounterEngine::set_game_state` takes `GameStateProvider::client_flags()`, the Conditions word alone, and `check_pull_end` applies one of three rules:
  - While the game has the local player in combat, only a wipe, a zone change or a manual end closes a pull. A boss's untargetable phase used to split its fight after 7 s of quiet.
  - Once the game ends combat, the pull closes `kCombatEndSettleSeconds` (2 s) after the later of the combat end and the last activity, as `CombatEnded`. The settle has two jobs. Party HP is read every `payload::intervals::PARTY_SYNC` (1.5 s), so a wipe that ends combat is still seen as a wipe; `orchestration_intervals.hpp` asserts that read, plus one loop tick, fits inside the settle. And a fight still landing hits isn't cut if the local player's own flag drops first.
  - When the game's state is unknown, a pull closes after `m_inactivity_timeout_seconds` (7 s, no longer a setting) without activity, as `Inactivity`. This covers no valid report for `kGameStateTtl` (3 s), an older payload that sends no `client_flags`, and a pull the game never had the local player in combat for, such as other players fighting nearby.

  The engine must never be fed `flags()`: that word folds the meter's own pull into `InCombat`, so a pull would hold itself open. A report without `Valid` is a failed read and is ignored. The game's state never starts a pull and is not activity. Duration still runs to the last combat activity (`m_last_activity_time - m_start_time`), never to when the end was noticed. The same trim applies to wipes and zone changes, and only an explicit `Manual` end takes the full elapsed time.
- **Registry State Is Recomputed, Never Only Set**: Nothing clears `CombatantRegistry` in production, so a flag or link that is only ever written outlives its source for the whole session. `sync_party` recomputes `is_party_member` for every actor (a member who left kept it and still passed the party-only filter), and `register_actor` erases a pet->owner link when the id re-registers as a non-pet (a recycled pet id otherwise merged a monster's damage into the old owner). A pet re-read without owner info keeps its link.
- **The Local Player Comes From The Client, Never Party Slot 0**: `CombatPartySyncPayload::local_player_id` carries the id the payload reads through `LOCAL_PLAYER_ENTITY_ID_*`, and `CombatantRegistry::sync_party` uses only that. Taking `entity_ids[0]` flagged whoever the server listed first as us, which moved `is_local_player` onto the wrong row and inferred ownerless pets to the wrong player. When the signature misses, the id is 0 (unknown), not a guess.
- **Solo Means Unknown Zone, Not The Last Duty**: The party list is the payload's only territory source, so an empty list (solo) publishes zone `0`, and a list whose members report no territory publishes nothing. Skipping the `0` left every post-duty dummy pull filed under the duty the DF party was in. A zone-only control packet (`in_combat_flag == 0 && control_command == EncounterControlCommand::None`) is authoritative for `0`; on any other control packet `0` means no zone. Going unknown mid-pull never ends the pull: `m_zone_unknown_pending` holds it until the pull is archived under the zone it was fought in, and only then does the unknown zone's visit begin.
- **Limit Break Is Nobody's DPS**: The game reports the casting player as the source of a Limit Break (confirmed against captured ability lines: `22|...|10FF0007|<player>|C5|Shield Wall|...`), so the action id is the only thing that identifies one. `is_limit_break_action` holds the game's ActionCategory 9 table; a match on a *friendly* source routes the damage to a synthetic `ActorType::LimitBreak` combatant at `hub::game::LIMIT_BREAK_COMBATANT_ID`. It counts toward `m_total_damage` and raid DPS but never toward a player's `total_damage`, `dps` or `damage_share_pct`. The friendly check matters because that category also holds duty-action and NPC limit breaks. The synthetic id sits above every real entity id and so carries the monster bit, which `is_friendly` must special-case before its monster test.
- **`control_command` Is Handled Though Never Sent**: `CombatControlPayload::control_command` is an `ipc::EncounterControlCommand`: `End` (1), `Reset` (2) or `Split` (3). No payload in the history has sent anything but `None`, but the values are on the wire, so the engine keeps acting on them and they are never renumbered or reused.
- **`0xE0000000` Is A Placeholder, Not An Actor**: `hub::game::NO_ENTITY_ID` fills the owner slot of an ownerless actor and the target slot of an effect that hit nothing - 891 such ability lines in one Zeromus EX clear. It is never a source, never a Limit Break, and must never open a combatant row; guard both slots with `is_real_entity_id`.
- **Mitigated Hits Are Still Damage**: `Blocked` and `Parried` effects carry a damage value and go through `record_damage_hit` like a full hit; only the block/parry counters are extra.
- **Ticks Carry No Severity**: `ProcessHotDot` reports no crit flag, so DoT/HoT ticks increment `HitCounts::tick_hits` and stay out of `rated_hits()`, the denominator for crit/DH/CDH rates. Heal hits are counted in `heal_hit_counts` for the same reason. Direct heals and HoT ticks book through the one `MetricsAccumulator::record_heal`, which counts a tick apart (`HealHit::Tick`) from a direct heal's crit or non-crit.
- **A Heal's Crit Is Byte 2**: Damage, blocked and parried hits keep crit and direct hit in `hit_severity` (byte 1, `0x20` and `0x40`). A heal keeps its crit in `param` (byte 2, `0x20`) and never direct hits; the client reads a heal's byte 1 only to pick a message. Reading byte 1 for heals counted no heal crit at all, so every heal crit rate read 0%. See "How the client reads an effect entry".
- **Only Effect Kinds 3 And 4 Are Ticks**: `ProcessHotDot` also carries MP (11) and job gauge (14) gains, and its last argument is an attack type or a flag, always `0` on the classic tick category. The detour classifies by kind alone through `hot_dot_is_heal`: 3 is damage, 4 is healing, and anything else is dropped before a consumer sees it. Deciding by that last argument booked every classic-category DoT tick as healing, and MP and Esprit gains with it. See "How the client reports DoT and HoT ticks".
- **Friendly Raid Damage Isolation**: Enemy incoming damage to players is tracked under `damage_taken` on the target. Enemy damage must never be added to `m_total_damage` or raid DPS.
- **Only Landed Damage Opens An Encounter**: `EncounterEngine::starts_encounter` gates the Idle -> InCombat transition on a `Damage`, `Blocked` or `Parried` effect carrying a non-zero damage value. Healing used to qualify, so a prepull cure opened a pull whose clock was already seconds old by the first hit, with the healer ranked in a table nobody had attacked in yet. Buffs, debuffs, misses and zero-damage effects are not a pull starting either. The rule governs the *start* only: once InCombat, a heal is recorded and refreshes `m_last_activity_time` like any other activity.
- **Direct Action Encounter Initiation Only**: Passive DoT/HoT ticks must never initiate encounters when combat state is Idle, Wipe, or Complete.
- **A Name The App Never Received Is Archived Forever**: The desktop app runs its own mirror `EncounterEngine` and learns a name only from a `CombatActorInfo` packet; `MetricsAccumulator` otherwise opens the row as `Entity_<id>`, and `end_encounter` snapshots it by value. Any path that resolves an actor from game memory must publish it, not just call `registry.register_actor` - that is what `ObjectReader::inspect_and_sync_actor{,_direct}` are for, and why `CombatPlugin` takes an actor resolver rather than reading the object itself. Solo is the case that exposes it: the party list is the only other publisher and it is empty.
- **Snapshots Are Throttled, Not Per Frame**: An `EncounterSummary` carries every combatant's per-action map. The in-game overlay and the desktop Combat view both cache one and refresh on an interval. The overlay and the payload's activity log line take `current_rankings()`, which leaves out the per-action maps and the detail rows they never read. List views use `pull_history_index()` (header fields only), not `pull_history()`.
- **Detail Rows Are Flat**: A summary's deaths, damage-taken rows and status uptime rows are plain values keyed by entity id, with the names they need in `names`. The desktop view copies a summary on every snapshot, so no row may carry a map or a per-row string.
- **Status Lists And Life Events Are Never Activity**: `process_status_list` and `process_life_event` never start a pull and never touch `m_last_activity_time`. A buff counting down or a party member lying dead is not combat, and treating either as activity would hold every pull open until the timers ran out.
- **The Vitals Pass Writes No HP**: `CombatPlugin::on_vitals` never calls `update_hp`. Wipe detection and the packet-derived in-combat bit keep exactly the inputs they had before, `sync_party` and actor info. An enemy's HP goes to `process_enemy_hp` and the boss readout, nowhere else.
- **A Death's Recap Is Built In-Game And Shipped**: Hits travel on the main-thread lane and life events on the orchestration lane, and order is FIFO per lane only, so the app cannot rebuild a recap from its own copy of the hits. `on_vitals` builds it with `build_life_event` from the payload engine's per-target recap rings and ships it inside `CombatLifeEvent`; both engines record exactly that. The life event goes out before the same actor's status list, so the death keeps the statuses the next list drops.
- **Late Deaths Join The Pull They Belong To**: A wipe can be detected from actor info on one lane before the deaths arrive on the other. A life event stamped after its pull was archived joins that pull if it is at most `kLateLifeEventUs` (5 s) past the pull's end, in the archive and, while the live data still describes that pull, in the live view.
- **A Status Loss Is Dated By Its Timer**: A status missing from a list ended at the earlier of the read and its own expected end, and never before the previous read. Dating every loss at the read added up to one poll interval to each expiring buff.
- **Enemy Statuses End With The Pull**: An enemy stops being tracked when its pull ends and its list is never read again, so `end_encounter` clears the status list of every actor that is not friendly. Otherwise the next pull's uptime opens with a stale DoT on a dead boss. The next pull can start on the same enemy before a vitals pass runs, so `on_vitals` resends every list whenever `pulls_started()` moves; without that, an unchanged list is never sent again and the new pull misses it.
- **A Departed Actor Gets One Empty List**: `VitalsTracker::retain` sends a single empty status list for an actor that left the pass (it left the party, or an enemy dropped out of the top four), which closes its statuses on the other side. `invalidate()` resends every list when the app reconnects, for the same reason as `ObjectReader::invalidate_cache()`.

## Boss And Pull Outcome Invariants

- **Enemy HP Feeds Only The Boss Readout**: `EncounterEngine::process_enemy_hp` hands a `CombatEnemyHp` read to `BossTracker` and does nothing else. It never starts a pull, never touches `m_last_activity_time`, and never writes the registry's HP. The payload sends one for a tracked enemy when its HP or max HP changed (`VitalsTracker::enemy_hp_changed`), and resends on a reconnect or a new pull, like the status lists.
- **The Boss Is The Damaged Enemy With The Most Max HP**: `BossTracker::boss` picks it from the enemies the pull damaged, the more damaged one on a tie. Max HP comes from the HP reads and falls back to the registry, so with vitals off a pull still names its boss but knows no HP. Only the four most damaged enemies are read, which is where a boss sits.
- **Clear Means The Boss's HP Read 0**: `pull_outcome` is the one rule for the badge. A boss whose last read was 0 makes the pull a Clear, even when the party died with it. Otherwise a wipe is a Wipe, and a pull the party survived is Ended when the boss's HP is known and Clear when it isn't, the badge every pull had before enemy HP was read. The desktop takes an archived pull's boss from `pull_history_index()`, since a late kill can change the pull after it was selected.
- **A Kill Stops The Clock At The Last Hit On An Enemy**: With the boss killed, `end_encounter` measures the pull to `m_last_enemy_hit_time`, the last damage from the party's side that landed on any enemy, whatever the end reason, Manual included; the end timestamp follows. Heals and HoT ticks after a kill are activity and hold the pull open until it closes, which stretched every clear by up to a HoT's length. It is the last hit on any enemy rather than on the boss, so a trash pull whose biggest mob falls first keeps the rest of its fight.
- **Only A Late Kill Joins The Archive**: A read stamped after its pull was archived changes that pull only if it is a 0 for its boss within `kLateLifeEventUs` of the end. Any other late read may already be the boss resetting after a wipe, and is dropped. A late kill does not re-trim the duration.

Whether a boss's object stands, at 0 HP, long enough for a vitals pass (every 250 ms) to read it is live-only. The client writes a target's HP when the effect result arrives (see "How the client reads an effect entry"), but despawning is the server's call. A boss gone before the next pass reads as Ended at the last HP seen.

## Pull History Invariants

- **Every Zone Change Starts A Visit**: `enter_zone_locked` starts a new `zone_visit`, a zone seen before included, and resets the visit's pull count. `set_zone_locked` calls it only after the ZoneChange `end_encounter_locked`, and `apply_pending_zone_locked` only once the held pull is archived, so a pull is always filed under the visit it was fought in. A reconnect republishes the same zone, which changes nothing. The picker groups by `zone_visit` (`group_pulls_by_visit`), never by `zone_id`: grouping by zone folded a duty entered twice into one list.
- **Pull Numbers Are Stamped When Archived**: `end_encounter_locked` sets `pull_number` from the visit's count and nothing renumbers it, so eviction at capacity leaves every number as it was. `encounter_id` still counts every pull and stays the key for selection and `timeline()`. `clear_history` starts the count again; the live summary has no number.
- **An Unknown Zone Keeps One Pull**: Archiving a pull in zone 0 first drops every earlier zone-0 pull, from any visit, and does it before the capacity check, so a run of solo pulls never evicts a duty's. Late deaths and kills only join `m_pull_history.back()`, which the drop never touches. A test that archives several pulls has to set a zone, or they collapse to one.
- **Only The App's Archive Is Sized By The Player**: `pull_history_limit` (10-500, default 100) reaches only the app's engine, through `AppState::set_pull_history_limit`: from `apply_config_to_mirror_engine` at start and on a reset, and from the Settings slider when it is released, never while it is dragged, since a drag through a low value would drop pulls for good. The payload's engine keeps one pull, set in `CombatPlugin`'s constructor: nothing in-game reads past `m_pull_history.back()`, and it used to hold the shared default, 25 full summaries the game never read. `set_history_capacity` drops the oldest at once and never goes below one, because the live view and late deaths and kills use the newest.

## Raid Buff Credit Invariants

- **Credits Are Built In-Game And Shipped**: `CombatPlugin` attributes every damage hit from a friendly source with `attribute_hit` before `process_action` and the ring-buffer push, the same place the overheal split happens, and ships the result in `CombatActionPayload::credits`. Both engines book exactly those credits and nothing downstream re-attributes. The statuses come from the `StatusReader` (`ObjectReader::read_attribution_statuses`) on the game's main thread at the hit; the app has no way to see them then, since status lists reach it at 4 Hz on another lane. See "How the client applies statuses".
- **Credits Move Damage, They Never Make It**: A hit's credits never add up to more than the hit (`record_credits` clamps them), go only to another player, and are booked on both the receiver (`buff_received`) and the giver (`buff_given`). The party's rDPS therefore sums to its DPS; `MeterRdps.CreditsMoveDamageWithoutCreatingIt` holds that. A player whose buffs are their only damage still ranks: `sorted_by_dps` and `CombatOverlay::sorted_combatants` keep a row with `buff_given > 0`. Heals, the Limit Break and a player's own buffs earn nothing.
- **A DoT Keeps The Buffs It Was Applied Under**: An effect entry of kind 14 on an enemy snapshots the attacker's statuses, keyed by target, status and source. `on_status_tick` credits a damage tick from that snapshot and the enemy's debuffs as they stand at the tick; a tick with no snapshot, or one older than `kDotSnapshotTtlUs`, earns nothing. Ticks carry no crit or direct hit, so rate buffs earn their expected value on them.
- **Rates Come From Clean Hits Only**: `RateEstimator` learns a player's crit and direct hit rates only from hits with no rate status on either side, no guarantee, and both status lists read. It starts from `kPriorCrit`/`kPriorDirectHit` at weight `kPriorWeight` and is clamped to `[kMinRate, kMaxRate]`. The crit multiplier is the rate plus 1.35, because one stat term drives both. It lives in the payload for the whole game session, so an app restart does not reset it.
- **Guaranteed Hits Turn Rates Into Damage**: A hit in `GUARANTEED_HIT_ACTIONS`, a GCD under a `GUARANTEED_HIT_STATUSES` status, or a form-bonus action under its form treats each rate buff as a damage multiplier of `1 + rate x bonus`. Both tables are generated from the sentence the game adds to such hits; do not hand-edit them.
- **Raid Buff Values Are Hand-Maintained**: `include/hub/game/raid_buffs.hpp` holds the status ids and strengths. The Status sheet names the statuses but carries no percentages; each value comes from its action's ActionTransient text. `MeterGameData.RaidBuffTableMatchesTheStatusSheet` pins every id to its sheet name. Strengths the applying action decides, Technical and Standard Finish by the finish used and Radiant Finale by the songs sung since the last one, are tracked by `BuffStrengths` from every action a player uses.

## Cast And GCD Uptime Invariants

- **A Cast Is A Button Press**: `decoder::decode_cast` makes one `CombatCast` per ActionEffect, however many targets it hit, when the header's `action_type` is `ACTION_TYPE_ACTION` and the action has a cooldown group (`game::is_pressed_action`) or is a Limit Break. Auto-attacks, pet actions and the effects the game fires by itself have no cooldown group; see "How the Action sheet marks a button". An item (a tincture) is another action type and never counts.
- **Only A Player's Own Casts Count**: `MetricsAccumulator::record_cast` drops a pet's cast, since the pet acts on its owner's press and that press counted already, and drops enemies and the Limit Break row. A pet merged into its owner late brings no casts and no GCDs with it.
- **Casts Never Start A Pull**: `process_cast` drops a cast outside a pull or stamped before the pull's start. `CombatPlugin` sends it after the same effect's `CombatAction` packets on the main-thread lane, so the press behind the hit that opened a pull counts in both engines. In a pull, a recorded cast is activity, like a heal: a phase where the party only buffs or dances holds the pull open.
- **GCD Uptime Is Not Worked Out On The Tick**: `gcd_uptime` walks every GCD cast of the pull, so `recalculate` only derives CPM. `update_gcd_uptime` runs for a summary with detail and when a pull is archived. The in-game overlay reads rankings, so the payload computes uptime once per pull. Computing it on the tick put a growing stall under the engine lock, which the game's main thread waits on. `GcdCast` keeps each cast's sheet timing so the pass does no table lookups.
- **A Hardcast Is Counted From Its Press**: A spell reaches the client when its cast ends (a cancelled cast sends no effect at all, see the Latency Mitigator's `AGENTS.md`). `gcd_uptime` moves a GCD with a sheet cast time back by that time, scaled to the estimated speed, when a hardcast fits: the previous GCD had come back (allowing 25% haste) and its own cast was over. Otherwise the spell was instant (Swiftcast, Dualcast, a proc) and counts where it landed. Counted where they landed, a Red Mage's hardcast and Dualcast pairs land 0.5 s and 4.5 s apart and read as about 62% uptime.
- **The GCD Estimate Only Uses Gaps That Cancel Out**: `estimate_gcd` takes gaps between two GCDs with the same sheet cast time, rescaled by the first one's recast to a 2.5 s GCD, keeps those between 1.5 and 3.6 s, and uses the 25th percentile. It stays at 2.5 s with fewer than 8 such gaps. A GCD's length is its own recast at that speed, so a 4 s motif holds its full recast.

## Timeline Invariants

- **Only The App's Engine Keeps A Timeline**: `EncounterEngine::set_timeline_enabled` is off by default and only `AppState` turns it on, so the payload's engine books no bins and builds no buff windows. The timeline costs the game nothing and needs no packet of its own: the app builds it from the actions, ticks and status lists its tables already use. It takes 24 bytes per party member per second, about 110 KB for a ten-minute pull of eight, 11 MB for the default archive of 100 of them and 55 MB at the 500 maximum.
- **One Bin Per Second, Booked With The Totals**: `MetricsAccumulator::timeline_bin` finds a `TimelineBin` (damage, effective healing, damage taken, and the buff damage given, received and received from single-target buffs) by the packet's timestamp from the pull's start. It is filled in the same record paths as the totals, never on the tick, and stops at `UINT32_MAX` rather than wrap. Nothing past `TIMELINE_MAX_SECONDS` (an hour) is binned; the totals still count it.
- **Only The Party Has A Timeline**: `keeps_timeline` holds for party members, the local player, and a pet not yet merged into its owner. The Limit Break, other players (an alliance raid's other parties, anyone nearby in the open world) and enemies get none, which keeps a pull to eight rows at most, whatever the content, plus a pet not merged yet. When `merge_combatants` merges a pet, its bins are added to its owner's, or dropped when the owner keeps none.
- **The Timeline Lives Beside The Summary**: An archived pull is an `ArchivedPull`, its `EncounterSummary` and its `EncounterTimeline`. A summary never carries the timeline, so `pull_at()`, the rankings and every snapshot cost what they did. It is read with `EncounterEngine::timeline(encounter_id)`, by id because an index shifts when the archive evicts, and 0 is the live pull. `take_timeline_rows` moves the bins into the archive and trims their spare capacity.
- **Buff Windows Are Merged Per Buff And Giver**: `StatusUptime::windows` takes the raid buffs in `raid_buffs.hpp` that reach the party, a buff on friendly targets or a debuff such as Chain Stratagem on an enemy, and leaves out cards and dance partner effects. Each status and source's intervals are merged over every target, bridging gaps under a second, since each target's list is read at a different moment.
- **The Chart Works When The Data Changes, Not Per Frame**: The Timeline tab fetches the live timeline every 250 ms while it is open, and an archived one once. It smooths each line and averages it down to one point per pixel column only when the data, a setting or the width changes, so a frame only draws. Lines are drawn at a whole-pixel width, which ImGui draws from its font texture with half the vertices.
- **The Chart Follows The Pull's Clock**: A line spans `EncounterSummary::duration_seconds`, so a kill trimmed to its last hit leaves the heals after it off the chart. `smoothed_series` makes two passes of half the window, which weighs the middle most: a hit slides out of the average instead of dropping out of it all at once.

## How the client marks combat

Read from `ffxiv_dx11.exe` on 2026-09-25 with `tools/inspect_exe.py`. Addresses are for
that build only; re-check after a patch.

- **One writer.** The Conditions array (`0x142ae65f0`, the `CONDITIONS_INSTANCE`
  signature) is written through `Conditions::Set` (`0x140e16030`, index below 112). Index
  26, `IN_COMBAT`, is set from exactly one immediate site: the ActorControl handler
  `0x140b3a5d4`, category 4 (jump table `0x140b47be4`, case `0x140b3a882`).
- **It is the server's word for the local player.** That case sets bit `0x2` of any
  character's flags at `+0x1D4` to `param1 == 1`. It sets Conditions[26] to the same only
  when `0x1408989b0` matches the character against the local player's `Character*`
  (`0x142aa0410`). The client infers nothing from targets or untargetable bosses.

What the client cannot say, and live play has to:

- That the server keeps the flag set through a boss's untargetable phase. The meter is
  built on it: a downtime that still splits a pull means it doesn't.
- Whether a dead player keeps the flag. The settle keeps a pull open while hits still
  land, so a fight is not cut either way.
- How long a striking dummy keeps the player in combat after the last hit.

## How the client keeps status lists

Read from `ffxiv_dx11.exe` on 2026-09-23 with `tools/inspect_exe.py`. Addresses are for
that build only; re-check after a patch.

- **Layout.** The StatusManager constructor (`0x1408a46e0`) zeroes the owner at `+0x0`,
  resets 60 slots of `0x10` bytes at `+0x8` (`u16 status, u16 param, float remaining,
  u64 source`, the source reset to `0xE0000000`), and sets the `u8` slot count at
  `+0x3D8` to 30. `SetStatus` (`0x1408a6fc0`) rejects a slot index of 60 or more and
  grows the count to 60. It null-checks the owner and then uses it as a `Character*`, so
  the owner is null or the owning character.
- **Characters keep theirs at `BattleChara + 0x23B0`.** Slot `0x278` of the vtable
  `0x1421a5ae8` (`GetStatusManager`) returns `this + 0x23B0`, and the BattleChara init runs
  the constructor on `r14 + 0x23B0` at `0x140afdc23`. It is a base-class member, so
  players and battle NPCs share it. The status-list packet handlers (`0x140b37f80`,
  `0x140b381d6`) write full data there, timers and sources included.
- **The party list's copy is second-rate.** The party list handler (`0x140b4a4b0`) fills
  the StatusManager at `PartyMember + 0x0` with timer 0 and source 0. The payload reads it
  only for a member out of range, and flags that list `STATUS_LIST_NO_DETAIL`.
- **The local player's object sits next to its id.** `0x142aa0410`, 8 bytes past the id
  global the local player signature resolves, holds its `Character*`. The id's initializer
  (`0x14004c8b0`) resets both together, and the character manager's delete paths
  (`0x140afde52`, `0x140afdf56`) null the pointer when that object is freed. The payload
  still checks the object's entity id and kind before trusting it. Solo, this is the only
  way to reach the player, since the party list is empty.
- **`GetObjectByEntityId` (`0x140b00830`) is a binary search that writes nothing.** The
  orchestration thread calls it while the main thread may be reshaping the array, so every
  object it returns is checked again by entity id and object kind, inside SEH.
- **The layout check.** A read passes when the slot count is 30 or 60 and the owner is
  null or the object itself. Twenty failures before any read has passed switch status
  reads off for the session, log once, and add "status reads off" to the payload status.
  Deaths keep working without them. After one read has passed, a failure is treated as a
  transient object and just skipped.

## How the client fills the party list

Read from `ffxiv_dx11.exe` on 2026-09-22 with `tools/inspect_exe.py` (see the
`inspect-game-client` skill). Addresses are for that build only; re-check after a patch.

- **Slots follow the server's order.** The party list packet handler (`0x140b4a4b0`)
  copies packet entry *i* into `Group` slot *i* (entity id, content id, HP, territory,
  job) and sets `MemberCount` to the packet's count through `SetMemberCount`
  (`0x140b25950`). Nothing reorders the list, and the leader index is sent separately.
- **The client finds "me" by id.** For each entry the handler compares the entity id with
  the local player id global (`0x142aa0408`, set next to the local player object pointer
  at `0x142aa0410`), and only runs some per-member work for entries that aren't us. Slot 0
  is not the local player.
- **No party means `MemberCount == 0`.** `Group::Clear` (`0x140b259b0`, called for both
  groups when `GroupManager` resets) zeroes the count and sets the leader index to -1, and
  a zone-init path calls `SetMemberCount(0)`. The client never inserts the local player
  into an empty list. Whether the server ever sends a one-entry list can't be read from
  the client; the territory vote is correct either way.
- **Lookups search the list.** `GetPartyMemberByContentId` (`0x140b26e50`) scans
  `MemberCount` slots at stride `0x490` comparing `+0x3F8`, and `GetPartyMemberByIndex`
  (`0x140b26dd0`) bounds the index by `MemberCount`. Most client checks read the count as
  "> 1 means grouped".

## How the client reads an effect entry

Read from `ffxiv_dx11.exe` on 2026-09-24 with `tools/inspect_exe.py`, and the LogMessage
sheet with `tools/xivdata`. Addresses are for that build only; re-check after a patch.

- **Effects are queued, then run.** `ReceiveActionEffect` (`0x140902eb0`) passes the
  header, effect blocks and target list through `0x140902d80` to `0x1409011b0`, which
  copies each target's eight entries unchanged into a 32-slot queue of `0x78`-byte
  records (`0x140901b60`). Running a record (`0x140901f40`) calls the per-effect handler
  `0x1408ffbf0` once per entry, which switches on the entry's first byte.
- **Byte 1 is the damage severity.** The damage case (`0x1408ffd52`) picks LogMessage
  505/511 ("Critical!") on `0x20`, 447/448 ("Direct hit!") on `0x40`, and 450/451
  ("Critical direct hit!") on both. The hit-effect function `0x1408fec50`, run for each
  entry just before, plays the crit effect on the same `0x20` for damage, blocked and
  parried hits. The battle log never shows a crit on a block or a parry.
- **A heal's crit is byte 2.** The heal case (`0x1409002cb`) picks LogMessage 520
  ("Critical! … recover(s) … HP.") over 519 on `byte[2] & 0x20`, and the hit-effect
  function's heal case (`0x1408ff3de`) plays its crit effect on the same bit. A heal's
  byte 1 is only compared with 1 and 3, which pick other message rows. Nothing reads a
  direct hit on a heal.
- **For damage, byte 2 is attack type and element.** Its low nibble goes through
  `0x1408e0160`, the mapping `0x605` ticks use for their last argument, and the high
  nibble picks the element's hit effect.
- **HP moves when the effect result arrives.** The effect-result handlers (HP stores at
  `0x140b38ffb` and `0x140b39397`) find the queued record by sequence and target
  (`0x140901dc0`), run it (`0x140902150`), and only then write the target's HP. The HP
  read when `ReceiveActionEffect` fires is the pre-heal value the overheal split needs.

## How the client reports DoT and HoT ticks

Read from `ffxiv_dx11.exe` on 2026-09-24 with `tools/inspect_exe.py`. Addresses are for
that build only; re-check after a patch.

- **One function, three packets.** `ProcessHotDot` (`0x1408a81d0`, the
  `PROCESS_HOT_DOT_PRIMARY` signature) is a StatusManager method. The ActorControl
  handler (`0x140b3a5d4`) reaches it through its jump table (`0x140b47be4`) for three
  categories:
  - `0x17`: kind = param2, amount = param3, source = param4, and a last argument of 0.
  - `0x604`: kind 4, amount = param2, source = param3, last argument = param4.
  - `0x605`: kind 3, amount = param2, source = param3, last argument = param4.
- **The kind is the game's effect numbering.** The function handles 3 (damage), 4
  (healing), 11 (MP gains such as Auto-ether and Sole Survivor), 14 (job gauge, such as
  Dancer's Esprit and Fan Dance) and 30. Only 3 and 4 are damage or healing.
- **The last argument is not a direction.** It is 0 for every `0x17` packet. On `0x605`
  it is an attack type: `0x1408e0160` maps 1-4 to one damage kind, 5 to another, and 0
  or anything else to a third, and a negative value skips it. On `0x604` a few HoT
  statuses use it to pick their flytext.
- **Amount and source are always the fifth and sixth arguments,** whichever packet
  called, and the second is the target Character. The detour forwards exactly those.
- **A tick moves no HP itself.** `ProcessHotDot` only prints the log line and flytext and
  plays a hit effect, and none of the three packets carries HP. The target's HP arrives
  in the HP/MP update packet (handler `0x140b38040`: `u32` HP, `u16` MP, `u16` GP, also
  copied into the party list), a status list or an effect result. Whether the server
  sends it before or after the tick is not in the client.
- **The tick split holds up under either order.** `on_status_tick` reads the target's HP
  at the hook, like an action. A tick on a target already at full reads full HP either
  way and is all overheal, which is right. If the HP update comes after the tick, the
  split is exact. If it comes first, a tick landing while the target is missing less than
  two ticks' worth of HP books up to one tick of extra overheal. Counting every tick as
  fully effective was off by all of its overheal.

## How the client applies statuses

Read from `ffxiv_dx11.exe` on 2026-09-24 with `tools/inspect_exe.py`, and the LogMessage
sheet with `tools/xivdata`. Addresses are for that build only; re-check after a patch.

- **Effect kinds 14 and 15 apply a status.** The per-effect handler's jump table
  (`inspect_exe.py jumptable 0x1409010a8 77 --index 0x140901134 --first 1`) sends both to
  `0x1409004ce`. It looks the entry's `value` (+6) up as a Status row and prints LogMessage
  526 or 527 ("gains/suffers the effect of"), picked by the row's debuff category. Kind 14
  prints it for the target, 15 for the caster.
- **Statuses change when their packet arrives, on the main thread.** The effect-result
  handler (`0x140b38f59`) runs the queued record, writes HP, then calls `SetStatus`
  (`0x1408a6fc0`) through the target's `GetStatusManager`. So when `ReceiveActionEffect`
  fires, every status the server applied before that hit is already in the StatusManager.
  The snapshot a hit is credited from is exact to packet order.
- **Header byte `0x1F` is 1 for an Action row.** `ReceiveActionEffect` compares it with 1
  (`0x140903601`) before treating the header's id as an action.

What the client cannot say, and live play has to:

- Whether Technical Finish, Standard Finish and Radiant Finale carry their strength in the
  status `param`. The payload logs each new value it sees for them once.
- Whether Life Surge and Reassembled are still on the attacker when its weaponskill's
  effect arrives. If not, that hit is credited as a rolled crit.
- That pets fight under their owner's buffs, that a DoT keeps the attacker's buffs from
  its application, and that enemy debuffs apply to each tick as it lands. The meter is
  built on all three.

## How the Action sheet marks a button

Read from the Action sheet with `tools/xivdata` on 2026-09-25. Column indices are pinned in
`tools/gen_game_tables.py` against known rows.

- **The GCD is cooldown group 58, as an action's own group (column 41) or its additional
  one (column 42).** An action with its own charges or cooldown keeps them in its own
  group and shares the GCD through the additional one: Drill, Air Anchor, Chain Saw,
  Gnashing Fang, Double Down, Standard and Technical Step, the first mudra of a Ninjutsu,
  Phlegma, Pneuma, Soul Slice and Vicewinder. `is_gcd_action` covers both, which also puts
  a Reassembled Drill among the guaranteed hits.
- **Recast and cast time are columns 40 and 38**, in 100 ms. Most GCDs recast in 2.5 s;
  some run 1.5 s (Ninjutsu, Emerald Rite, the enchanted Riposte), 0.5 s (a mudra after the
  first) or longer (Ruby Rite 3.0 s, a motif 4.0 s, Rainbow Drip 6.0 s). An action whose
  own group is not the GCD gets 2.5 s in `gcd_timing`, which the sheet does not record.
- **Every button has a cooldown group; nothing else does.** Auto-attacks (7 and 8), pet
  actions (Wyrmwave, Pile Bunker), and the rows for effects the game fires by itself have
  group 0 and no recast: Kardia's heal (28119), Eudaimonia (37036), Liturgy of the Bell's
  triggers (25863, 25864) and Stellar Explosion (7441). So do the party's Limit Breaks,
  which is why `decode_cast` names them apart.

What the sheet cannot say, and live play has to:

- That the server sends those rows' ids for the effects they describe. If it sent Kardia's
  heal under Kardia's own id (24285), each heal would count as a cast.
- Whether an effect the game fires later arrives under the pressed action's own id, such
  as Wildfire's detonation or Excogitation's heal. If it does, it counts as a second cast.
