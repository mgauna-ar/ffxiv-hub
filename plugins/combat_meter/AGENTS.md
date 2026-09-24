# Combat Meter: Agent Guidelines

Plugin-local rules. The cross-cutting ones - single `ReceiveActionEffect` hook and its
phase ordering, IPC and ring buffer safety, hook lifecycle and teardown, tray and
dashboard decoupling - live in the root [`AGENTS.md`](../../AGENTS.md) and apply here too.

Two meter invariants deliberately stay in the root as well, because code outside this
directory can violate them: `EncounterEngine` is shared state reached from four threads,
and derived rates are recomputed on the tick rather than per packet.

## Combat Analytics Invariants

Every rule below records a failure that was observed and fixed, several of them against
captured log lines. Preserve them when modifying the registry, accumulator or engine.

- **Automatic Pet Attribution (Zero Orphan Rows)**: `CombatantRegistry::resolve_owner` must map pet actions to the owner. Unlinked pets infer owners from party jobs. Stats merge cleanly with zero orphan rows. Cyclic ownership loop guard depth = 8. On a late merge the owner's `pet_damage` grows by the merged row's `total_damage` alone (its own `pet_damage` is already inside it), and a missing owner row is created from the registry, never by moving the pet's row across.
- **Effect Blocks Map To `targets[t]`**: Every effect block, slot 0 included, belongs to the matching entry of the target list; `animation_target_id` is only the fallback for a null list or empty slot. A self-centred AoE animates on its caster.
- **Safe Duration Floor (Anti-Division-by-Zero)**: Combat duration must be clamped to `std::max(duration_seconds, 1.0)`.
- **Accurate Overheal Accounting**: HPS is strictly `effective_healing / duration`. Total healing is `effective_healing + overhealing`. Overheal percentage evaluates against total healing without division-by-zero when healing is zero.
- **Overheal Is Split Before Anything Records It**: The decoder emits a heal as all effective. `CombatPlugin` splits it with `decoder::apply_overheal` against the target's pre-heal HP from its `HpResolver` *before* `process_action` and the ring-buffer push, so the in-game and desktop engines hold identical numbers. Nothing downstream may re-split. A heal flagged on-source (`flags & 0x80`) targets the caster.
- **Party Wipe State Invariance**: A wipe triggers only when all tracked synced party members are confirmed dead (`party_dead == m_party_members.size()`). A surviving player or revive cancels the wipe. *Confirmed dead* is `max_hp > 0 && current_hp == 0`: an unread HP proves nothing, and `register_actor` keeps a real 0 as 0. Solo, the party is the local player alone: with no synced party the check reads `m_local_player_id` and nothing else. Counting every player ever registered let a former party member, a stranger or a pet last read alive block every solo wipe. With no local id the old all-players count stands, pets excluded.
- **HP Must Keep Flowing**: Wipe detection is only as live as the HP it reads. `ObjectReader::sync_party` calls `update_hp` on every sync, and the actor cache's dedupe carries an alive/dead bit so a death or raise republishes `CombatActorInfo` to the app. Taking HP out of that path, or out of the dedupe entirely, silently turns every wipe into an inactivity end. Solo the party list is empty, so `sync_party` reads the local player's own object instead and publishes it through the same dedupe.
- **7.0s Inactivity Timeout & Duration Accuracy**: Encounters auto-split after 7.0 seconds without combat activity. Duration is calculated from the time of the last combat activity (`m_last_activity_time - m_start_time`), not inflated by the 7.0s timeout window. The same trim applies to wipes and zone changes, which are also detected after the fact; only an explicit `Manual` end takes the full elapsed time.
- **Registry State Is Recomputed, Never Only Set**: Nothing clears `CombatantRegistry` in production, so a flag or link that is only ever written outlives its source for the whole session. `sync_party` recomputes `is_party_member` for every actor (a member who left kept it and still passed the party-only filter), and `register_actor` erases a pet->owner link when the id re-registers as a non-pet (a recycled pet id otherwise merged a monster's damage into the old owner). A pet re-read without owner info keeps its link.
- **The Local Player Comes From The Client, Never Party Slot 0**: `CombatPartySyncPayload::local_player_id` carries the id the payload reads through `LOCAL_PLAYER_ENTITY_ID_*`, and `CombatantRegistry::sync_party` uses only that. Taking `entity_ids[0]` flagged whoever the server listed first as us, which moved `is_local_player` onto the wrong row and inferred ownerless pets to the wrong player. When the signature misses, the id is 0 (unknown), not a guess.
- **Solo Means Unknown Zone, Not The Last Duty**: The party list is the payload's only territory source, so an empty list (solo) publishes zone `0`, and a list whose members report no territory publishes nothing. Skipping the `0` left every post-duty dummy pull filed under the duty the DF party was in. A zone-only control packet (`in_combat_flag == 0 && control_command == 0`) is authoritative for `0`; on any other control packet `0` means no zone. Going unknown mid-pull never ends the pull: `m_zone_unknown_pending` holds it until the pull is archived under the zone it was fought in.
- **Limit Break Is Nobody's DPS**: The game reports the casting player as the source of a Limit Break (confirmed against captured ability lines: `22|...|10FF0007|<player>|C5|Shield Wall|...`), so the action id is the only thing that identifies one. `is_limit_break_action` holds the game's ActionCategory 9 table; a match on a *friendly* source routes the damage to a synthetic `ActorType::LimitBreak` combatant at `hub::game::LIMIT_BREAK_COMBATANT_ID`. It counts toward `m_total_damage` and raid DPS but never toward a player's `total_damage`, `dps` or `damage_share_pct`. The friendly check matters because that category also holds duty-action and NPC limit breaks. The synthetic id sits above every real entity id and so carries the monster bit, which `is_friendly` must special-case before its monster test.
- **`0xE0000000` Is A Placeholder, Not An Actor**: `hub::game::NO_ENTITY_ID` fills the owner slot of an ownerless actor and the target slot of an effect that hit nothing - 891 such ability lines in one Zeromus EX clear. It is never a source, never a Limit Break, and must never open a combatant row; guard both slots with `is_real_entity_id`.
- **Mitigated Hits Are Still Damage**: `Blocked` and `Parried` effects carry a damage value and go through `record_damage_hit` like a full hit; only the block/parry counters are extra.
- **Ticks Carry No Severity**: `ProcessHotDot` reports no crit flag, so DoT/HoT ticks increment `HitCounts::tick_hits` and stay out of `rated_hits()`, the denominator for crit/DH/CDH rates. Heal hits are counted in `heal_hit_counts` for the same reason.
- **Only Effect Kinds 3 And 4 Are Ticks**: `ProcessHotDot` also carries MP (11) and job gauge (14) gains, and its last argument is an attack type or a flag, always `0` on the classic tick category. The detour classifies by kind alone through `hot_dot_is_heal`: 3 is damage, 4 is healing, and anything else is dropped before a consumer sees it. Deciding by that last argument booked every classic-category DoT tick as healing, and MP and Esprit gains with it. See "How the client reports DoT and HoT ticks".
- **Friendly Raid Damage Isolation**: Enemy incoming damage to players is tracked under `damage_taken` on the target. Enemy damage must never be added to `m_total_damage` or raid DPS.
- **Only Landed Damage Opens An Encounter**: `EncounterEngine::starts_encounter` gates the Idle -> InCombat transition on a `Damage`, `Blocked` or `Parried` effect carrying a non-zero damage value. Healing used to qualify, so a prepull cure opened a pull whose clock was already seconds old by the first hit, with the healer ranked in a table nobody had attacked in yet. Buffs, debuffs, misses and zero-damage effects are not a pull starting either. The rule governs the *start* only: once InCombat, a heal is recorded and refreshes `m_last_activity_time` like any other activity.
- **Direct Action Encounter Initiation Only**: Passive DoT/HoT ticks must never initiate encounters when combat state is Idle, Wipe, or Complete.
- **A Name The App Never Received Is Archived Forever**: The desktop app runs its own mirror `EncounterEngine` and learns a name only from a `CombatActorInfo` packet; `MetricsAccumulator` otherwise opens the row as `Entity_<id>`, and `end_encounter` snapshots it by value. Any path that resolves an actor from game memory must publish it, not just call `registry.register_actor` - that is what `ObjectReader::inspect_and_sync_actor{,_direct}` are for, and why `CombatPlugin` takes an actor resolver rather than reading the object itself. Solo is the case that exposes it: the party list is the only other publisher and it is empty.
- **Snapshots Are Throttled, Not Per Frame**: An `EncounterSummary` carries every combatant's per-action map. The in-game overlay and the desktop Combat view both cache one and refresh on an interval. The overlay and the payload's activity log line take `current_rankings()`, which leaves out the per-action maps and the detail rows they never read. List views use `pull_history_index()` (header fields only), not `pull_history()`.
- **Detail Rows Are Flat**: A summary's deaths, damage-taken rows and status uptime rows are plain values keyed by entity id, with the names they need in `names`. The desktop view copies a summary on every snapshot, so no row may carry a map or a per-row string.
- **Status Lists And Life Events Are Never Activity**: `process_status_list` and `process_life_event` never start a pull and never touch `m_last_activity_time`. A buff counting down or a party member lying dead is not combat, and treating either as activity would hold every pull open until the timers ran out.
- **The Vitals Pass Writes No HP**: `CombatPlugin::on_vitals` never calls `update_hp`. Wipe detection and the packet-derived in-combat bit keep exactly the inputs they had before, `sync_party` and actor info.
- **A Death's Recap Is Built In-Game And Shipped**: Hits travel on the main-thread lane and life events on the orchestration lane, and order is FIFO per lane only, so the app cannot rebuild a recap from its own copy of the hits. `on_vitals` builds it with `build_life_event` from the payload engine's per-target recap rings and ships it inside `CombatLifeEvent`; both engines record exactly that. The life event goes out before the same actor's status list, so the death keeps the statuses the next list drops.
- **Late Deaths Join The Pull They Belong To**: A wipe can be detected from actor info on one lane before the deaths arrive on the other. A life event stamped after its pull was archived joins that pull if it is at most `kLateLifeEventUs` (5 s) past the pull's end, in the archive and, while the live data still describes that pull, in the live view.
- **A Status Loss Is Dated By Its Timer**: A status missing from a list ended at the earlier of the read and its own expected end, and never before the previous read. Dating every loss at the read added up to one poll interval to each expiring buff.
- **Enemy Statuses End With The Pull**: An enemy stops being tracked when its pull ends and its list is never read again, so `end_encounter` clears the status list of every actor that is not friendly. Otherwise the next pull's uptime opens with a stale DoT on a dead boss. The next pull can start on the same enemy before a vitals pass runs, so `on_vitals` resends every list whenever `pulls_started()` moves; without that, an unchanged list is never sent again and the new pull misses it.
- **A Departed Actor Gets One Empty List**: `VitalsTracker::retain` sends a single empty status list for an actor that left the pass (it left the party, or an enemy dropped out of the top four), which closes its statuses on the other side. `invalidate()` resends every list when the app reconnects, for the same reason as `ObjectReader::invalidate_cache()`.

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
  Dancer's Esprit and Fan Dance) and 30. Only 3 and 4 move HP.
- **The last argument is not a direction.** It is 0 for every `0x17` packet. On `0x605`
  it is an attack type: `0x1408e0160` maps 1-4 to one damage kind, 5 to another, and 0
  or anything else to a third, and a negative value skips it. On `0x604` a few HoT
  statuses use it to pick their flytext.
- **Amount and source are always the fifth and sixth arguments,** whichever packet
  called, and the second is the target Character. The detour forwards exactly those.
