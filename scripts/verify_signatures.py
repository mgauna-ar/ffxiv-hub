#!/usr/bin/env python3
"""Check the AOB signatures in include/hub/game_definitions.hpp against a game exe.

Signatures are the part of the payload that silently rots across game patches,
and the only other way to test one is to launch the game on Windows. This scans
ffxiv_dx11.exe offline and reports, per signature, how many times it matches.

    python3 scripts/verify_signatures.py <path to ffxiv_dx11.exe>

A signature that resolves a static singleton must match exactly once and land in
a data section; a function signature must match exactly once in .text.
"""

import re
import struct
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFINITIONS = REPO_ROOT / "include" / "hub" / "game_definitions.hpp"

# Signatures whose match is a RIP-relative LEA for a static instance, mapped to
# (displacement offset, instruction length). Everything else is a function.
RIP_RELATIVE = {
    "ACTION_MANAGER_INSTANCE_PRIMARY": (3, 7),
    "ACTION_MANAGER_INSTANCE_FALLBACK": (3, 7),
    "GAME_OBJECT_MANAGER_INSTANCE": (3, 7),
    "GROUP_MANAGER_INSTANCE": (5, 9),
    "CONDITIONS_INSTANCE": (3, 7),
}


def parse_sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("not a PE executable")
    n_sections = struct.unpack_from("<H", data, pe + 6)[0]
    opt_size = struct.unpack_from("<H", data, pe + 20)[0]
    image_base = struct.unpack_from("<Q", data, pe + 24 + 24)[0]

    table = pe + 24 + opt_size
    sections = []
    for i in range(n_sections):
        entry = data[table + i * 40: table + (i + 1) * 40]
        sections.append({
            "name": entry[:8].rstrip(b"\0").decode(),
            "vsize": struct.unpack_from("<I", entry, 8)[0],
            "rva": struct.unpack_from("<I", entry, 12)[0],
            "rsize": struct.unpack_from("<I", entry, 16)[0],
            "raw": struct.unpack_from("<I", entry, 20)[0],
        })
    return image_base, sections


def section_for_rva(sections, rva):
    for s in sections:
        if s["rva"] <= rva < s["rva"] + max(s["vsize"], s["rsize"]):
            return s["name"]
    return None


def parse_pattern(pattern):
    values, mask = [], []
    for token in pattern.split():
        if token.startswith("?"):
            values.append(0)
            mask.append(False)
        else:
            values.append(int(token, 16))
            mask.append(True)
    return bytes(values), mask


def find_all(blob, pattern):
    values, mask = parse_pattern(pattern)
    n = len(values)
    hits, start = [], 0
    first = values[0:1]
    while True:
        i = blob.find(first, start)
        if i < 0 or i + n > len(blob):
            break
        if all(not mask[j] or blob[i + j] == values[j] for j in range(n)):
            hits.append(i)
        start = i + 1
    return hits


def load_signatures():
    source = DEFINITIONS.read_text()
    body = source[source.index("namespace signatures"):]
    body = body[:body.index("} // namespace signatures")]
    pattern = re.compile(
        r'constexpr\s+std::string_view\s+(\w+)\s*=\s*\n?\s*"([0-9A-Fa-f? ]+)"\s*;')
    return pattern.findall(body)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2

    exe = Path(sys.argv[1]).expanduser()
    if not exe.is_file():
        print(f"error: {exe} is not a file")
        return 2

    data = exe.read_bytes()
    image_base, sections = parse_sections(data)
    text = next(s for s in sections if s["name"] == ".text")
    blob = data[text["raw"]: text["raw"] + text["rsize"]]

    print(f"{exe.name}: image base {image_base:#x}, .text {text['rsize']:,} bytes\n")

    failures = 0
    for name, pattern in load_signatures():
        hits = find_all(blob, pattern)
        status = "ok " if len(hits) == 1 else "FAIL"
        if len(hits) != 1:
            failures += 1
        print(f"[{status}] {name}: {len(hits)} match(es)")

        if len(hits) == 1 and name in RIP_RELATIVE:
            disp_offset, insn_len = RIP_RELATIVE[name]
            insn_rva = text["rva"] + hits[0]
            disp = struct.unpack_from("<i", blob, hits[0] + disp_offset)[0]
            target_rva = insn_rva + insn_len + disp
            where = section_for_rva(sections, target_rva) or "<outside any section>"
            print(f"         instruction {image_base + insn_rva:#x}"
                  f" -> target {image_base + target_rva:#x} in {where}")
            if where == ".text" or where is None:
                print("         warning: a static instance should resolve into a data section")
                failures += 1

    print()
    if failures:
        print(f"{failures} signature(s) need attention.")
        return 1
    print("All signatures matched uniquely.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
