---
name: after-game-patch
description: Regenerate game data tables and repair AOB signatures after a Final Fantasy XIV patch. Use when the game has updated, a Dawntrail version bump lands, generated tables need regenerating, or a signature check reports BROKEN or AMBIGUOUS - including any work involving tools/gen_game_tables.py, tools/check_signatures.py, or the offsets and signatures in include/hub/game_definitions.hpp.
---

# After a Game Patch

Six headers are generated from the game's own Excel sheets, and the payload's signatures
are byte patterns in the game executable. Both can go stale on patch day.

Everything here is offline - no XIVAPI, no third-party data - and needs only files copied
from an install.

## 1. Copy the game files

Only the `0a0000` (exd) category and the executable are read, ~375 MB total. From the
Windows install root (`C:\Program Files (x86)\SquareEnix\FINAL FANTASY XIV - A Realm Reborn\`):

```
game/sqpack/ffxiv/0a0000.win32.*     # .index, .index2 and every datN
game/ffxiv_dx11.exe
```

The other category prefixes are models, textures, sound and scripts; the `ex1`...`ex5`
folders hold expansion content. Excel sheets all live in the base `ffxiv` repository,
current expansion included. Put them anywhere outside the repo - both tools default to
`$FFXIV_HUB_GAME_DIR`, then `~/ffxiv/game`, and take `--game-dir`.

The two halves use different files: step 2 reads only the exd data, step 3 reads only the
executable. A signature never depends on the `datN` files.

## 2. Regenerate the data tables

```bash
python3 tools/gen_game_tables.py
```

Writes `include/hub/game/{actions,status,territory,limit_break,job,guaranteed_hits}.hpp`
and `src/common/src/game_tables.cpp`. Review `git diff` on those: it shows exactly which
actions, statuses, duties and jobs the patch added or renamed. Regeneration is
deterministic - an unchanged install must produce byte-identical files.

`game_tables.cpp` also holds the player GCD table behind `is_gcd_action` and `gcd_timing`:
Action rows with a job level (column 12, 0 on NPC actions) in cooldown group 58, as their
own group (column 41) or their additional one (column 42), each with its recast (column 40)
and cast time (column 38) in 100 ms. The level and group columns are pinned against Heavy
Swing (31: level 1, group 58), Fell Cleave (3549: level 54) and Berserk (38: group 11);
the other three against Glare III (25859: 1.5 s cast), Jolt III (37004: 2.0 s) and Drill
(16498: 20 s recast in group 5, additional group 58). Check Drill and Standard Step (15997)
are still in the table.

It also holds the set behind `is_pressed_action`, the Action rows with any cooldown group.
That is what tells a button press from an auto-attack, a pet action or an effect the game
fires by itself; check Kardia's heal (28119) and the auto-attacks (7 and 8) stay out of it.
`MeterGameData.GcdAndPressTablesComeFromTheSheet` pins those ids by name.

`guaranteed_hits.hpp` comes from the sentence the game adds to every guaranteed crit or
direct hit ("...increased when under an effect that raises critical hit rate [or direct
hit rate]"): ActionTransient for attacks, filtered to those that deliver a hit, and the
Status sheet for statuses such as Life Surge and Reassembled. Check the diff lists only
attacks and the statuses that grant a guarantee; a rewording of that sentence empties it.

`game_tables.cpp` also holds the debuff set behind `status_is_detrimental`: the Status
sheet's category column, where 1 is a buff and 2 a debuff. It is pinned against Battle
Litany (786, a buff) and Vulnerability Up (638, a debuff); check both in the diff.

- **Action and Status are large** (~45k and ~4.8k rows), so they live in one `.cpp` with
  only a declaration in the header. Do not move them into a header: pulling 1.6 MB into
  every translation unit is the difference between a 37s build and a much worse one.
- **Their names are `sv` literals.** A plain literal makes the constant evaluator walk
  each name to find its length, and the Action table alone then exceeds clang's default
  `-fconstexpr-steps`; the `sv` suffix passes the length in. Keep it if you touch
  `gen_tables_cpp`, and check `make CXX=clang++` as well as g++.
- **`pets.hpp` is deliberately NOT generated.** The game's `Pet` sheet also lists every
  Beastmaster tameable (`squirrel`, `crab`, `bat`, `ghost`, `behemoth`, `chimera`...),
  whose names collide with ordinary enemies; generating it would merge bosses into player
  rows. It stays hand-curated, and matching is exact - a substring test previously
  classified every Titan/Garuda/Ifrit/Bahamut boss as a pet and dropped it from the meter.
- **Column indices are positional**, pinned at the top of the generator and verified
  against known rows (Braver/200 in ActionCategory 9, territory 1238 ->
  "Futures Rewritten (Ultimate)"). If a patch reorders sheet columns the generator will
  emit plausible-looking wrong data rather than fail, so check those spot-values in the
  diff, and re-pin the indices if they moved.
- **`ClassJob.Role` does not match this project's `Role`**: the game does not split
  physical ranged from casters. `job_to_role` derives that from the job-role column (where
  2 and 6 are both healer subtypes) and lets a base class inherit from the job that grows
  out of it. Verified to reproduce the hand-written table exactly for every pre-existing job.

The generator reads the archives through `tools/xivdata/sqpack.py` (SqPack `.index`
lookup and `datN` extraction) and `tools/xivdata/excel.py` (`.exh` header and `.exd` row
pages). Both were verified against patch 7.56. String cells are SeStrings: inline macros
are stripped except Hyphen (`0x1F`), which the sheet uses mid-name ("Tam-Tara") and is
decoded to `-`.

## 3. Check the signatures

```bash
python3 tools/check_signatures.py                 # ~/ffxiv/game, or $FFXIV_HUB_GAME_DIR
python3 tools/check_signatures.py --exe <path to ffxiv_dx11.exe>
```

Reads the patterns straight out of `game_definitions.hpp` - no second copy to drift - and
matches them with the same `hub::memory::find_pattern` the payload uses, against `.text`
in the executable on disk. Each is `OK` (exactly one hit), `BROKEN` (zero) or `AMBIGUOUS`
(more than one, which is just as bad since the payload takes the first match).
`tools/xivbin/pe.py` finds `.text`, and a compiled `tools/sigcheck/sigcheck.cpp` does the
matching, because `pe_scanner` only works on a module the OS has loaded.

Every signature that resolves a static global is also followed to its target: the tool
reads that signature's `<SIGNATURE>_RIP_DISP_OFFSET` / `<SIGNATURE>_RIP_INSN_END` pair
from the header, decodes the displacement and confirms the target lands in a data
section rather than `.text` (`NOT DATA` otherwise). A pattern can match exactly once and
still be the wrong instruction, or the pair can point at the wrong bytes; this catches
both. Non-zero exit if any signature fails either check.

**What it cannot do:** it reports that a signature broke, not what to replace it with,
and it cannot validate the struct field offsets. Both remain manual reversing work - the
rest of this section is how to do it.

## Repairing a broken signature

### The two-tier system is the recovery method

Signatures come in two kinds. PRIMARY patterns are call-sites - they start `E8 ? ? ? ?`
and the 32-bit relative displacement is resolved at runtime to reach the target. FALLBACK
patterns are the function's own prologue bytes.

Five targets carry both tiers:

- `RECEIVE_ACTION_EFFECT_PRIMARY` / `_FALLBACK`
- `USE_ACTION_LOCATION_PRIMARY` / `_FALLBACK`
- `ACTION_MANAGER_INSTANCE_PRIMARY` / `_FALLBACK`
- `GET_OBJECT_BY_ENTITY_ID` / `_FALLBACK`
- `LOCAL_PLAYER_ENTITY_ID_PRIMARY` / `_FALLBACK`. The two tiers resolve the same global
  through different instructions, so each has its own `LOCAL_PLAYER_ENTITY_ID_*_RIP_*` pair.

When one tier breaks, the other usually still resolves. Use the surviving tier to locate
the function in the copied executable, then re-cut the broken pattern from the bytes at
that address. Nothing external is needed. A call-site broken by compiler reordering gets a
verified-unique prologue as its replacement, and vice versa.

### Four signatures have no fallback

`PROCESS_HOT_DOT_PRIMARY`, `GAME_OBJECT_MANAGER_INSTANCE`, `GROUP_MANAGER_INSTANCE` and
`CONDITIONS_INSTANCE` are single-pattern. If one of those breaks there is nothing to
recover from, and locating the function is ordinary disassembly of the copied exe. The
`inspect-game-client` skill covers it: find a unique instruction that references the
same global (`tools/inspect_exe.py xrefs`) and re-cut the pattern from its bytes.

### AMBIGUOUS is a different fix

An ambiguous pattern is not wrong, it is too short or too generic for the new binary.
Extend it with more bytes from the same instruction sequence until it is unique again,
rather than replacing it wholesale.

### Update these together

- `game::signatures::*` - the pattern itself.
- `game::offsets::*` - if struct members shifted.
- `<SIGNATURE>_RIP_DISP_OFFSET` and `<SIGNATURE>_RIP_INSN_END` - if the displacement's
  position inside the match, or the end of its instruction, moved. Every signature that
  resolves a static global has this pair, named after it and counted from the start of
  the match, so a re-cut pattern that starts earlier or later shifts both. A new such
  signature gets its pair too; `tools/check_signatures.py` finds it by name, so a
  missing pair means its target is never checked.
- `SUPPORTED_GAME_VERSION` - bump it; nothing else records which patch the definitions
  target.
- The struct padding, so every `static_assert(offsetof(...))` still passes. Those asserts
  are the only automated check on the offsets, so keeping them compiling is the whole
  verification story for a field move.

## 4. Re-check the action dispatch model

The Latency Mitigator's timing rules rest on how the client sends an action, recorded in
[How the client dispatches an action](../../../plugins/latency_mitigator/AGENTS.md#how-the-client-dispatches-an-action).
A patch can change that without breaking a signature, so re-check it against the new
executable before trusting those rules. `tools/inspect_exe.py` (see `inspect-game-client`)
disassembles the copied exe; find the hooked `UseActionLocation` through the resolved
`USE_ACTION_LOCATION_PRIMARY` call site and confirm:

- It returns 0 when `[mgr+0x08]` (animation lock) is above 0.
- It increments `word [mgr+0x120]` before sending, and writes 0.5f to `[mgr+0x08]` after.
- Queueing is a separate helper that only writes `+0x68`/`+0x6C`/`+0x70`/`+0x78`.
- The dequeue in `ActionManager::Update` calls `UseAction` (which reaches the hooked
  function) and clears `+0x68` only after that call returns.

If any of these changed, update that section and the rules built on it before changing the
mitigator's timing math.

## 4b. Re-check the status list layout

The meter reads status lists at `game::offsets::BATTLE_CHARA_STATUS_MANAGER`, and the
local player's object at `LOCAL_PLAYER_OBJECT_FROM_ID` past its id global. No signature
covers either, so a patch that moves them breaks nothing loudly. The payload's layout
check switches status reads off and the payload status shows "status reads off".
Re-verify both against the new executable with `tools/inspect_exe.py`, following
[How the client keeps status lists](../../../plugins/combat_meter/AGENTS.md#how-the-client-keeps-status-lists):

- The BattleChara vtable's `GetStatusManager` slot (`0x278` in 7.x) still reads
  `lea rax,[rcx+0x23B0]; ret`. The same offset appears as the `lea rcx,[reg+0x23B0]` in
  front of the StatusManager constructor call in the BattleChara init.
- The StatusManager constructor still lays out 60 slots of `0x10` at `+0x8`, and still
  sets the slot count at `+0x3D8` to 30. `SetStatus` still bounds the slot index by 60.
- The local player's `Character*` still sits 8 bytes past the id global that
  `LOCAL_PLAYER_ENTITY_ID_*` resolves. `xrefs <id global> --span 0x10` shows the two
  written side by side.

Update the offsets and the `StatusManagerObject` padding together; its `static_assert`s
pin the slots and the count.

## 4c. Re-check the tick kinds

The meter tells a DoT tick from a HoT tick by `ProcessHotDot`'s fourth argument, the
effect kind, as recorded in
[How the client reports DoT and HoT ticks](../../../plugins/combat_meter/AGENTS.md#how-the-client-reports-dot-and-hot-ticks).
A patch can renumber it without breaking the signature. Disassemble the function
`PROCESS_HOT_DOT_PRIMARY` resolves to and confirm:

- It still branches on the fourth argument, with 3 taking the damage path and 4 the
  healing one (`HOT_DOT_KIND_DAMAGE`, `HOT_DOT_KIND_HEAL`).
- Its callers in the ActorControl handler (`xrefs <function>`) still pass the amount
  fifth and the source sixth.

If either moved, update the constants and that section together.

## 4d. Re-check the lobby marker

Every overlay stays hidden while the local player id global reads `0xE0000000`, as
recorded in [How the client marks the lobby](../../../AGENTS.md#how-the-client-marks-the-lobby).
A patch can change when the client writes that value without breaking the signature. Its
stores show up in two places: `xrefs <id global> --span 4` for the static initializer,
and `field <offset in Control> --writes` (`0x7698` in 7.x) for the constructor, the
reset and the zone-init setter. Confirm:

- `0xE0000000` is still stored only by the constructor, its inlined copy in the static
  initializer, the `Control` reset, and the zone-init setter's fallback for a missing
  source object. The reset is still reached only from `GameMain`'s reset.
- The zone-init setter is still the only store of a real id, and is still reached only
  from the InitZone handler.

If the client now writes `0xE0000000` while a character is in the world, the overlays
vanish there. Fix `ObjectReader::in_lobby()` and that section before shipping the patch.

## 4e. Re-check the effect entry bits

The decoder reads crit and direct hit from fixed bits of each `ActionEffectEntry`, as
recorded in
[How the client reads an effect entry](../../../plugins/combat_meter/AGENTS.md#how-the-client-reads-an-effect-entry).
A patch can move them without breaking a signature. From the function
`RECEIVE_ACTION_EFFECT_*` resolves to, follow the call that takes the header, effect
blocks and target list down to the per-effect handler, the one switching on the entry's
first byte, and confirm:

- The heal case (type 4) picks LogMessage 520 ("Critical!") over 519 on `byte[2] & 0x20`.
- The damage case (type 3) picks its "Critical!" and "Direct hit!" rows on `byte[1]`'s
  `0x20` and `0x40`.

Read the row texts from the LogMessage sheet with `tools/xivdata`, since the same patch
can renumber them. If a bit moved, update the decoder's constants, the `ActionEffectEntry`
comments and that section together.

## 4f. Re-check the raid buffs

`include/hub/game/raid_buffs.hpp` is hand-maintained: the Status sheet names each raid buff
but carries no percentage. After a patch:

- `make` runs `MeterGameData.RaidBuffTableMatchesTheStatusSheet`, which fails when an id
  no longer names the buff it is listed as.
- Read each buff action's ActionTransient text with `tools/xivdata` and compare its
  percentage with the table, including the dance finish strengths and Radiant Finale's
  per-coda value. Job changes land here without breaking anything.
- A new raid buff needs an entry, sorted by status id, and a line in that test. The
  Timeline tab's buff bands come from the same table, so a buff missing from it has no
  band either.

The meter also depends on effect kinds 14 and 15 being status applications, as recorded
in [How the client applies statuses](../../../plugins/combat_meter/AGENTS.md#how-the-client-applies-statuses).
Re-run `tools/inspect_exe.py jumptable` on the per-effect handler's switch (see 4e for how
to find it) and confirm both still reach the handler that prints "gains the effect of".

## 5. Verify

```bash
make
```

Live verification needs Windows and the game running: enable `dry_run` in the Latency
Mitigator settings, then confirm the hooks attach and telemetry streams while no game
memory is written.
