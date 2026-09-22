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

- **Automatic Pet Attribution (Zero Orphan Rows)**: `CombatantRegistry::resolve_owner` must map pet actions to the owner. Unlinked pets infer owners from party jobs. Stats merge cleanly with zero orphan rows. Cyclic ownership loop guard depth = 8.
- **Safe Duration Floor (Anti-Division-by-Zero)**: Combat duration must be clamped to `std::max(duration_seconds, 1.0)`.
- **Accurate Overheal Accounting**: HPS is strictly `effective_healing / duration`. Total healing is `effective_healing + overhealing`. Overheal percentage evaluates against total healing without division-by-zero when healing is zero.
- **Party Wipe State Invariance**: A wipe triggers only when all tracked synced party members are confirmed dead (`party_dead == m_party_members.size()`). A surviving player or revive cancels the wipe.
- **7.0s Inactivity Timeout & Duration Accuracy**: Encounters auto-split after 7.0 seconds without combat activity. Duration is calculated from the time of the last combat activity (`m_last_activity_time - m_start_time`), not inflated by the 7.0s timeout window. The same trim applies to wipes and zone changes, which are also detected after the fact; only an explicit `Manual` end takes the full elapsed time.
- **Limit Break Is Nobody's DPS**: The game reports the casting player as the source of a Limit Break (confirmed against captured ability lines: `22|...|10FF0007|<player>|C5|Shield Wall|...`), so the action id is the only thing that identifies one. `is_limit_break_action` holds the game's ActionCategory 9 table; a match on a *friendly* source routes the damage to a synthetic `ActorType::LimitBreak` combatant at `hub::game::LIMIT_BREAK_COMBATANT_ID`. It counts toward `m_total_damage` and raid DPS but never toward a player's `total_damage`, `dps` or `damage_share_pct`. The friendly check matters because that category also holds duty-action and NPC limit breaks. The synthetic id sits above every real entity id and so carries the monster bit, which `is_friendly` must special-case before its monster test.
- **`0xE0000000` Is A Placeholder, Not An Actor**: `hub::game::NO_ENTITY_ID` fills the owner slot of an ownerless actor and the target slot of an effect that hit nothing - 891 such ability lines in one Zeromus EX clear. It is never a source, never a Limit Break, and must never open a combatant row; guard both slots with `is_real_entity_id`.
- **Mitigated Hits Are Still Damage**: `Blocked` and `Parried` effects carry a damage value and go through `record_damage_hit` like a full hit; only the block/parry counters are extra.
- **Ticks Carry No Severity**: `ProcessHotDot` reports no crit flag, so DoT/HoT ticks increment `HitCounts::tick_hits` and stay out of `rated_hits()`, the denominator for crit/DH/CDH rates. Heal hits are counted in `heal_hit_counts` for the same reason.
- **Friendly Raid Damage Isolation**: Enemy incoming damage to players is tracked under `damage_taken` on the target. Enemy damage must never be added to `m_total_damage` or raid DPS.
- **Only Landed Damage Opens An Encounter**: `EncounterEngine::starts_encounter` gates the Idle -> InCombat transition on a `Damage`, `Blocked` or `Parried` effect carrying a non-zero damage value. Healing used to qualify, so a prepull cure opened a pull whose clock was already seconds old by the first hit, with the healer ranked in a table nobody had attacked in yet. Buffs, debuffs, misses and zero-damage effects are not a pull starting either. The rule governs the *start* only: once InCombat, a heal is recorded and refreshes `m_last_activity_time` like any other activity.
- **Direct Action Encounter Initiation Only**: Passive DoT/HoT ticks must never initiate encounters when combat state is Idle, Wipe, or Complete.
- **A Name The App Never Received Is Archived Forever**: The desktop app runs its own mirror `EncounterEngine` and learns a name only from a `CombatActorInfo` packet; `MetricsAccumulator` otherwise opens the row as `Entity_<id>`, and `end_encounter` snapshots it by value. Any path that resolves an actor from game memory must publish it, not just call `registry.register_actor` - that is what `ObjectReader::inspect_and_sync_actor{,_direct}` are for, and why `CombatPlugin` takes an actor resolver rather than reading the object itself. Solo is the case that exposes it: the party list is the only other publisher and it is empty.
- **Snapshots Are Throttled, Not Per Frame**: An `EncounterSummary` carries every combatant's per-action map. The in-game overlay and the desktop Combat view both cache one and refresh on an interval. List views use `pull_history_index()` (header fields only), not `pull_history()`.

