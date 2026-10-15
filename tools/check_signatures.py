#!/usr/bin/env python3
"""Checks the payload's signatures against a real ffxiv_dx11.exe.

Patterns are read out of include/hub/game_definitions.hpp so there is no second
copy to drift, and matched with the same scanner the payload uses. Run this after
a game patch: it says which signatures broke, though not what to replace them with.

    python3 tools/check_signatures.py [--game-dir ~/ffxiv/game] [--exe path/to/ffxiv_dx11.exe]

Every signature must match exactly once in .text. One that resolves a static
global, which is one with a <NAME>_RIP_DISP_OFFSET / <NAME>_RIP_INSN_END pair in
the header, must also land in a data section: a target in .text, or in no section
at all, means the operand is not where the pair says. So must one whose operand is
an offset from the image base, named by a <NAME>_IMAGE_DISP_OFFSET. Exit status is
non-zero when any signature fails either check.
"""

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from xivbin.pe import Image  # noqa: E402

REPO = os.path.dirname(HERE)
HEADER = os.path.join(REPO, "include", "hub", "game_definitions.hpp")
SOURCES = [
    os.path.join(HERE, "sigcheck", "sigcheck.cpp"),
    os.path.join(REPO, "src", "common", "src", "sigscan.cpp"),
]

# constexpr std::string_view NAME =\n    "AA BB ? ...";
PATTERN_RE = re.compile(
    r'constexpr\s+std::string_view\s+(\w+)\s*=\s*"([0-9A-Fa-f?\s]+)"\s*;', re.S
)
# constexpr size_t NAME_RIP_DISP_OFFSET = 3;  /  NAME_RIP_INSN_END = 7;
RIP_RE = re.compile(r"constexpr\s+size_t\s+(\w+)_RIP_(DISP_OFFSET|INSN_END)\s*=\s*(\w+)\s*;")
# constexpr size_t NAME_IMAGE_DISP_OFFSET = 6;
IMAGE_RE = re.compile(r"constexpr\s+size_t\s+(\w+)_IMAGE_DISP_OFFSET\s*=\s*(\w+)\s*;")


def _namespace_body(source, name):
    start = source.index(f"namespace {name}")
    end = source.index(f"}} // namespace {name}", start)
    return source[start:end]


def read_signatures():
    source = open(HEADER, encoding="utf-8").read()
    return PATTERN_RE.findall(_namespace_body(source, "signatures"))


def read_rip_operands():
    """{signature name: (disp offset, instruction end)} for each signature that
    resolves a static global. The suffixes are unique, so the whole header is read."""
    source = open(HEADER, encoding="utf-8").read()
    fields = {}
    for name, field, value in RIP_RE.findall(source):
        fields.setdefault(name, {})[field] = int(value, 0)
    operands = {}
    for name, pair in fields.items():
        if set(pair) != {"DISP_OFFSET", "INSN_END"}:
            sys.exit(f"{name} has only half of its RIP pair in {HEADER}")
        operands[name] = (pair["DISP_OFFSET"], pair["INSN_END"])
    return operands


def read_image_operands():
    """{signature name: disp offset} for each signature whose operand is an offset
    from the image base."""
    source = open(HEADER, encoding="utf-8").read()
    return {name: int(value, 0) for name, value in IMAGE_RE.findall(source)}


def report_target(img, name, match_va, target):
    """Prints where an operand lands; 1 when that is not a data section."""
    where = img.section_of(target)
    print(f"  {'':<36} {match_va:#x} -> {target:#x} in {where or '<outside any section>'}")
    if where is None or where == ".text":
        print(f"  {'':<36} NOT DATA: a static global should resolve into a data section")
        return 1
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        "--game-dir",
        default=os.environ.get("FFXIV_HUB_GAME_DIR", os.path.expanduser("~/ffxiv/game")),
        help="directory holding ffxiv_dx11.exe (default: $FFXIV_HUB_GAME_DIR or ~/ffxiv/game)",
    )
    parser.add_argument("--exe", help="path to ffxiv_dx11.exe, instead of looking in --game-dir")
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()

    exe = os.path.expanduser(args.exe) if args.exe else os.path.join(args.game_dir, "ffxiv_dx11.exe")
    if not os.path.isfile(exe):
        sys.exit(f"no ffxiv_dx11.exe at {exe}")

    signatures = read_signatures()
    if not signatures:
        sys.exit(f"no signatures found in {HEADER}")
    rip_operands = read_rip_operands()
    image_operands = read_image_operands()
    unknown = sorted((set(rip_operands) | set(image_operands)) - {name for name, _ in signatures})
    if unknown:
        sys.exit(f"operand offsets with no signature of that name in {HEADER}: {', '.join(unknown)}")

    img = Image(exe)
    if ".text" not in img.sections:
        sys.exit(f"no .text section in {exe}")
    text_rva, text_vsize, text_raw, text_rawsize = img.sections[".text"]
    text_size = min(text_vsize, text_rawsize)
    text_va = img.base + text_rva

    binary = os.path.join(os.environ.get("TMPDIR", "/tmp"), "ffxiv_hub_sigcheck")
    build = [args.cxx, "-std=c++20", "-O2", f"-I{REPO}/include", f"-I{REPO}/src/common/include",
             *SOURCES, "-o", binary]
    if subprocess.call(build) != 0:
        sys.exit("failed to build the signature checker")

    print(f"checking {len(signatures)} signatures against {exe}")
    print(f"image base {img.base:#x}, .text {text_size:,} bytes\n")

    out = subprocess.run(
        [binary, exe, str(text_raw), str(text_size)] + [f"{n}={p}" for n, p in signatures],
        capture_output=True, text=True)
    if out.returncode != 0:
        sys.stderr.write(out.stderr)
        sys.exit("the signature checker failed")

    failures = 0
    for line in out.stdout.splitlines():
        name, hits, first = line.split("\t")
        hits, first = int(hits), int(first)
        if hits < 0:
            verdict = "UNPARSEABLE"
        else:
            verdict = "OK" if hits == 1 else ("BROKEN" if hits == 0 else "AMBIGUOUS")
        failures += verdict != "OK"
        count = "" if hits < 0 else f"({hits} hit{'' if hits == 1 else 's'})"
        print(f"  {name:<36} {verdict:<10} {count}")

        match_va = text_va + first
        if verdict == "OK" and name in rip_operands:
            disp_offset, insn_end = rip_operands[name]
            failures += report_target(img, name, match_va, img.rip_target(match_va, disp_offset, insn_end))
        elif verdict == "OK" and name in image_operands:
            failures += report_target(img, name, match_va, img.image_target(match_va, image_operands[name]))

    print(f"\n{'all signatures resolve uniquely' if failures == 0 else 'SOME SIGNATURES NEED ATTENTION'}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
