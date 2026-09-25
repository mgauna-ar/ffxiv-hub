#!/usr/bin/env python3
"""Static questions about ffxiv_dx11.exe, answered from the local install.

    python3 tools/inspect_exe.py sig GROUP_MANAGER_INSTANCE --rip 5 9
    python3 tools/inspect_exe.py func 0x140b4a4b0
    python3 tools/inspect_exe.py disasm 0x140b26dd0 0x140b26df0
    python3 tools/inspect_exe.py xrefs 0x140b259b0          # callers of a function
    python3 tools/inspect_exe.py xrefs 0x142aa0400 --span 0x20   # uses of a global
    python3 tools/inspect_exe.py field 0x7fdc --writes      # stores to a struct offset
    python3 tools/inspect_exe.py jumptable 0x1409010a8 77 --index 0x140901134 --first 1

`sig` takes a name from include/hub/game_definitions.hpp or a literal pattern.
Addresses are VAs at the image base the exe declares (0x140000000).
"""

import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from check_signatures import read_signatures
from xivbin.pe import Image


def num(text):
    return int(text, 0)


def label(img, func):
    return f"{func:#x}" if func else "(leaf)"


def cmd_sig(img, args):
    pattern = dict(read_signatures()).get(args.pattern, args.pattern)
    hits = img.find_pattern(pattern)
    print(f"{len(hits)} match(es) for {pattern}")
    for hit in hits:
        line = f"  {hit:#x}"
        if args.rip:
            ins = hit + args.at
            target = img.rip_target(ins, args.rip[0], args.rip[1])
            line += f"  -> {target:#x} ({img.section_of(target)})"
        print(line)


def cmd_func(img, args):
    func, lines = img.disasm_func(args.va)
    if func:
        print(f"; function {func[0]:#x}-{func[1]:#x} ({func[1] - func[0]} bytes)")
    else:
        print("; no .pdata entry (leaf function?), showing a window")
    print("\n".join(lines))


def cmd_disasm(img, args):
    print("\n".join(img.disasm(args.start, args.stop)))


def cmd_xrefs(img, args):
    refs = img.xrefs(args.va, args.span)
    print(f"{len(refs)} reference(s) into {args.va:#x}+{args.span:#x}")
    for func, line in refs:
        print(f"  in {label(img, func)}  {line}")


def cmd_field(img, args):
    rows = img.field_accesses(args.offset, writes_only=args.writes)
    kind = "stores to" if args.writes else "accesses of"
    print(f"{len(rows)} {kind} [reg + {args.offset:#x}]")
    for func, line in rows:
        print(f"  in {label(img, func)}  {line}")


def cmd_jumptable(img, args):
    cases = img.jump_table(args.targets, args.count, args.index, args.first)
    print(f"{args.count} case(s), {len(cases)} handler(s)")
    for target, values in sorted(cases.items(), key=lambda kv: kv[1][0]):
        print(f"  {target:#x}  {', '.join(str(v) for v in values)}")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        "--game-dir",
        default=os.environ.get("FFXIV_HUB_GAME_DIR", os.path.expanduser("~/ffxiv/game")),
        help="directory holding ffxiv_dx11.exe (default: $FFXIV_HUB_GAME_DIR or ~/ffxiv/game)",
    )
    sub = parser.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("sig", help="match a signature, optionally resolving a RIP operand")
    p.add_argument("pattern", help="signature name from game_definitions.hpp, or a pattern")
    p.add_argument("--at", type=num, default=0, help="instruction offset from the match start")
    p.add_argument("--rip", type=num, nargs=2, metavar=("DISP_OFF", "INS_END"),
                   help="displacement offset and instruction end, from the match (or --at)")
    p.set_defaults(fn=cmd_sig)

    p = sub.add_parser("func", help="disassemble the function containing an address")
    p.add_argument("va", type=num)
    p.set_defaults(fn=cmd_func)

    p = sub.add_parser("disasm", help="disassemble an address range")
    p.add_argument("start", type=num)
    p.add_argument("stop", type=num)
    p.set_defaults(fn=cmd_disasm)

    p = sub.add_parser("xrefs", help="calls, jumps and RIP-relative uses of an address")
    p.add_argument("va", type=num)
    p.add_argument("--span", type=num, default=1, help="bytes from va to include (a struct)")
    p.set_defaults(fn=cmd_xrefs)

    p = sub.add_parser("jumptable", help="which case values of a switch reach which handler")
    p.add_argument("targets", type=num, help="VA of the dword table of image-relative targets")
    p.add_argument("count", type=num, help="number of cases: the compare bound plus one")
    p.add_argument("--index", type=num, help="VA of the byte table folding cases, if the switch has one")
    p.add_argument("--first", type=num, default=0, help="case value the switch subtracted before indexing")
    p.set_defaults(fn=cmd_jumptable)

    p = sub.add_parser("field", help="instructions using a struct offset ([reg + off])")
    p.add_argument("offset", type=num)
    p.add_argument("--writes", action="store_true", help="only stores")
    p.set_defaults(fn=cmd_field)

    args = parser.parse_args()
    exe = os.path.join(args.game_dir, "ffxiv_dx11.exe")
    if not os.path.exists(exe):
        sys.exit(f"no ffxiv_dx11.exe under {args.game_dir}")
    args.fn(Image(exe), args)


if __name__ == "__main__":
    main()
