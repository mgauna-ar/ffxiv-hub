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
- **Party Wipe State Invariance**: A wipe triggers only when all tracked synced party members are confirmed dead (`party_dead == m_party_members.size()`). A surviving player or revive cancels the wipe. *Confirmed dead* is `max_hp > 0 && current_hp == 0`: an unread HP proves nothing, and `register_actor` keeps a real 0 as 0.
- **HP Must Keep Flowing**: Wipe detection is only as live as the HP it reads. `ObjectReader::sync_party` calls `update_hp` on every sync, and the actor cache's dedupe carries an alive/dead bit so a death or raise republishes `CombatActorInfo` to the app. Taking HP out of that path, or out of the dedupe entirely, silently turns every wipe into an inactivity end.
- **7.0s Inactivity Timeout & Duration Accuracy**: Encounters auto-split after 7.0 seconds without combat activity. Duration is calculated from the time of the last combat activity (`m_last_activity_time - m_start_time`), not inflated by the 7.0s timeout window. The same trim applies to wipes and zone changes, which are also detected after the fact; only an explicit `Manual` end takes the full elapsed time.
- **Registry State Is Recomputed, Never Only Set**: Nothing clears `CombatantRegistry` in production, so a flag or link that is only ever written outlives its source for the whole session. `sync_party` recomputes `is_party_member` for every actor (a member who left kept it and still passed the party-only filter), and `register_actor` erases a pet->owner link when the id re-registers as a non-pet (a recycled pet id otherwise merged a monster's damage into the old owner). A pet re-read without owner info keeps its link.
- **The Local Player Comes From The Client, Never Party Slot 0**: `CombatPartySyncPayload::local_player_id` carries the id the payload reads through `LOCAL_PLAYER_ENTITY_ID_*`, and `CombatantRegistry::sync_party` uses only that. Taking `entity_ids[0]` flagged whoever the server listed first as us, which moved `is_local_player` onto the wrong row and inferred ownerless pets to the wrong player. When the signature misses, the id is 0 (unknown), not a guess.
- **Solo Means Unknown Zone, Not The Last Duty**: The party list is the payload's only territory source, so an empty list (solo) publishes zone `0`, and a list whose members report no territory publishes nothing. Skipping the `0` left every post-duty dummy pull filed under the duty the DF party was in. A zone-only control packet (`in_combat_flag == 0 && control_command == 0`) is authoritative for `0`; on any other control packet `0` means no zone. Going unknown mid-pull never ends the pull: `m_zone_unknown_pending` holds it until the pull is archived under the zone it was fought in.
- **Limit Break Is Nobody's DPS**: The game reports the casting player as the source of a Limit Break (confirmed against captured ability lines: `22|...|10FF0007|<player>|C5|Shield Wall|...`), so the action id is the only thing that identifies one. `is_limit_break_action` holds the game's ActionCategory 9 table; a match on a *friendly* source routes the damage to a synthetic `ActorType::LimitBreak` combatant at `hub::game::LIMIT_BREAK_COMBATANT_ID`. It counts toward `m_total_damage` and raid DPS but never toward a player's `total_damage`, `dps` or `damage_share_pct`. The friendly check matters because that category also holds duty-action and NPC limit breaks. The synthetic id sits above every real entity id and so carries the monster bit, which `is_friendly` must special-case before its monster test.
- **`0xE0000000` Is A Placeholder, Not An Actor**: `hub::game::NO_ENTITY_ID` fills the owner slot of an ownerless actor and the target slot of an effect that hit nothing - 891 such ability lines in one Zeromus EX clear. It is never a source, never a Limit Break, and must never open a combatant row; guard both slots with `is_real_entity_id`.
- **Mitigated Hits Are Still Damage**: `Blocked` and `Parried` effects carry a damage value and go through `record_damage_hit` like a full hit; only the block/parry counters are extra.
- **Ticks Carry No Severity**: `ProcessHotDot` reports no crit flag, so DoT/HoT ticks increment `HitCounts::tick_hits` and stay out of `rated_hits()`, the denominator for crit/DH/CDH rates. Heal hits are counted in `heal_hit_counts` for the same reason.
- **Friendly Raid Damage Isolation**: Enemy incoming damage to players is tracked under `damage_taken` on the target. Enemy damage must never be added to `m_total_damage` or raid DPS.
- **Only Landed Damage Opens An Encounter**: `EncounterEngine::starts_encounter` gates the Idle -> InCombat transition on a `Damage`, `Blocked` or `Parried` effect carrying a non-zero damage value. Healing used to qualify, so a prepull cure opened a pull whose clock was already seconds old by the first hit, with the healer ranked in a table nobody had attacked in yet. Buffs, debuffs, misses and zero-damage effects are not a pull starting either. The rule governs the *start* only: once InCombat, a heal is recorded and refreshes `m_last_activity_time` like any other activity.
- **Direct Action Encounter Initiation Only**: Passive DoT/HoT ticks must never initiate encounters when combat state is Idle, Wipe, or Complete.
- **A Name The App Never Received Is Archived Forever**: The desktop app runs its own mirror `EncounterEngine` and learns a name only from a `CombatActorInfo` packet; `MetricsAccumulator` otherwise opens the row as `Entity_<id>`, and `end_encounter` snapshots it by value. Any path that resolves an actor from game memory must publish it, not just call `registry.register_actor` - that is what `ObjectReader::inspect_and_sync_actor{,_direct}` are for, and why `CombatPlugin` takes an actor resolver rather than reading the object itself. Solo is the case that exposes it: the party list is the only other publisher and it is empty.
- **Snapshots Are Throttled, Not Per Frame**: An `EncounterSummary` carries every combatant's per-action map. The in-game overlay and the desktop Combat view both cache one and refresh on an interval. List views use `pull_history_index()` (header fields only), not `pull_history()`.

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
