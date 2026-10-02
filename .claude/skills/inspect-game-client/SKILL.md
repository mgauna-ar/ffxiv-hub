---
name: inspect-game-client
description: Answer questions about how the FFXIV client behaves or lays out memory by reading the local ffxiv_dx11.exe and SqPack data in ~/ffxiv/game, instead of assuming. Use before any change that rests on a client-behaviour premise - party list order or count, local player identity, object table, entity ids, action dispatch or queueing, conditions, struct offsets, what a global holds, who writes a field - and whenever you need values from the game's Excel sheets (.dat). Covers tools/inspect_exe.py (signatures, functions, callers, xrefs, field stores), tools/check_signatures.py and tools/xivdata.
---

# Inspecting The Game Client

A premise about the client is unverified until it has been checked in the binary. Two
regressions came from assuming instead: the mitigator's queued-action model, and the
combat meter treating party slot 0 as the local player. The install is at `~/ffxiv/game`
(`$FFXIV_HUB_GAME_DIR` overrides it); nothing here runs the game.

## Tools

| Question | Command |
|---|---|
| Does a signature still match, and where does it point? | `python3 tools/inspect_exe.py sig GROUP_MANAGER_INSTANCE` (a literal pattern takes `--rip <disp offset> <insn end>`) |
| Check all signatures after a patch | `python3 tools/check_signatures.py`: every signature matches once, and each static-global one lands in a data section, following the `<SIGNATURE>_RIP_*` pair in `game_definitions.hpp`. See `after-game-patch` |
| Show a function | `python3 tools/inspect_exe.py func 0x140b4a4b0` |
| Show a range (leaf functions have no `.pdata` entry) | `python3 tools/inspect_exe.py disasm 0x140b26dd0 0x140b26df0` |
| Who calls this? | `python3 tools/inspect_exe.py xrefs 0x140b259b0` |
| Who reads/writes this global or struct? | `python3 tools/inspect_exe.py xrefs 0x142aa0400 --span 0x20` |
| Who writes this struct offset? | `python3 tools/inspect_exe.py field 0x7fdc --writes` (drop `--writes` for reads) |
| Which case of a switch reaches which handler? | `python3 tools/inspect_exe.py jumptable 0x1409010a8 77 --index 0x140901134 --first 1`: the dword table of targets, the case count, the byte table folding shared cases when there is one, and the value the switch subtracted |
| Game sheet data | `tools/xivdata`: `Sheet(SqPack(game_dir + "/sqpack/ffxiv"), "TerritoryType").rows()`. String cells come back with SeString macros stripped, except Hyphen (`0x1F`) as `-` |

`inspect_exe.py` wraps `tools/xivbin/pe.py`, which is importable for anything the CLI does
not cover (`Image.find_pattern`, `rip_target`, `func_containing`, `xrefs`,
`field_accesses`, `jump_table`). Disassembly uses an `llvm-objdump` with the x86 backend;
`/usr/bin/objdump` on macOS has it. The xref scan is compiled from `tools/xivbin/xrefs.cpp`
into `$TMPDIR` on first use.

Addresses are only valid for the patch you read them from. Never put a VA in code: code
reaches the client through a signature in `include/hub/game_definitions.hpp`, and
`static_assert`ed offsets.

## Recipes

**Find a manager's instance.** Resolve its signature with `sig NAME`. It follows the
signature's `<NAME>_RIP_DISP_OFFSET` and `<NAME>_RIP_INSN_END` in `game_definitions.hpp`,
counted from the match start, the pair the payload passes to `resolve_rip_relative`; a
re-cut pattern not in the header yet takes them as `--rip <disp offset> <insn end>`. The
target lands in `.data`.

**Find what fills a field.** `field <offset> --writes` lists the stores. A field set
through a helper shows no direct store; look for a small function whose only job is that
store (`SetMemberCount` writes `+0x7FDC` from its argument), then `xrefs` it for callers.

**Identify an unknown global.** Run `xrefs <va> --span 4`. Its initializer shows the reset
value (`0xE0000000` means an entity id), and what the compares check it against shows its
meaning. Neighbouring globals initialized in the same function are usually one struct.

**Follow a packet handler.** A handler reads fields from its packet argument at fixed
offsets and writes them into client structs. The loop shape tells you the mapping, for
example "entry i goes to slot i", which is exactly the kind of fact premises depend on.

## Limits

- **Server behaviour is not in the exe.** What the server sends, like whether a solo
  player ever gets a one-entry party list or whether a pet's id is reused for a monster,
  can't be read from the client. Say so, and design for both answers.
- **Leaf functions have no unwind info.** `func` falls back to a window, and `xrefs` into
  one are found by call target rather than by function.
- **Findings belong next to the code they justify, not here.** Record a verified model in
  the relevant `AGENTS.md`, with the patch it was read from. The mitigator's lives in
  "How the client dispatches an action". In the combat meter's `AGENTS.md`: combat state
  in "How the client marks combat", status lists and the party list in "How the client
  keeps status lists" and "How the client fills the party list", effect entries, ticks
  and status applications in "How the client reads an effect entry", "How the client
  reports DoT and HoT ticks" and "How the client applies statuses", and GCD/button flags
  in "How the Action sheet marks a button". In the root `AGENTS.md`: "How the client
  keeps a character's worlds", "How the client builds an aetheryte", "How the client
  marks the lobby", and "How the client keeps its window and swap chain".
