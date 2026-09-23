---
name: after-game-patch
description: Regenerate game data tables and repair AOB signatures after a Final Fantasy XIV patch. Use when the game has updated, a Dawntrail version bump lands, generated tables need regenerating, or a signature check reports BROKEN or AMBIGUOUS - including any work involving tools/gen_game_tables.py, tools/check_signatures.py, scripts/verify_signatures.py, or the offsets and signatures in include/hub/game_definitions.hpp.
---

# After a Game Patch

Five headers are generated from the game's own Excel sheets, and the payload's signatures
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

Writes `include/hub/game/{actions,status,territory,limit_break,job}.hpp` and
`src/common/game_tables.cpp`. Review `git diff` on those: it shows exactly which actions,
statuses, duties and jobs the patch added or renamed. Regeneration is deterministic - an
unchanged install must produce byte-identical files.

`game_tables.cpp` also holds the debuff set behind `status_is_detrimental`: the Status
sheet's category column, where 1 is a buff and 2 a debuff. It is pinned against Battle
Litany (786, a buff) and Vulnerability Up (638, a debuff); check both in the diff.

- **Action and Status are large** (~45k and ~4.8k rows), so they live in one `.cpp` with
  only a declaration in the header. Do not move them into a header: pulling 1.6 MB into
  every translation unit is the difference between a 37s build and a much worse one.
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
pages). Both were verified against patch 7.56.

## 3. Check the signatures

Run both. They check different things and neither subsumes the other.

```bash
python3 tools/check_signatures.py
```

Reads the patterns straight out of `game_definitions.hpp` - no second copy to drift - and
matches them with the same `hub::memory::find_pattern` the payload uses, against `.text`
in the executable on disk. Each is `OK` (exactly one hit), `BROKEN` (zero) or `AMBIGUOUS`
(more than one, which is just as bad since the payload takes the first match). Non-zero
exit if any is not `OK`. It compiles `tools/sigcheck/sigcheck.cpp` to do the matching,
because `pe_scanner` only works on a module the OS has loaded.

```bash
python3 scripts/verify_signatures.py <path to ffxiv_dx11.exe>
```

Adds a check the first one has no notion of: for the five signatures that resolve a static
singleton, it decodes the RIP-relative displacement and confirms the target lands in a
data section rather than `.text`. A pattern can match exactly once and still be the wrong
instruction; this catches that. It is pure Python and needs no C++ toolchain.

**What neither can do:** they report that a signature broke, not what to replace it with,
and they cannot validate the struct field offsets. Both remain manual reversing work - the
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
  through different instructions, so each has its own `LOCAL_PLAYER_ID_*_RIP_*` pair.

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
- `*_RIP_DISP_OFFSET` - if the displacement's position inside the instruction moved.
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

## 5. Verify

```bash
make
```

Live verification needs Windows and the game running: enable `dry_run` in the Latency
Mitigator settings, then confirm the hooks attach and telemetry streams while no game
memory is written.
