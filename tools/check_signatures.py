#!/usr/bin/env python3
"""Checks the payload's signatures against a real ffxiv_dx11.exe.

Patterns are read out of include/hub/game_definitions.hpp so there is no second
copy to drift, and matched with the same scanner the payload uses. Run this after
a game patch: it says which signatures broke, though not what to replace them with.

    python3 tools/check_signatures.py [--game-dir ~/ffxiv/game]

Exit status is non-zero when any signature does not resolve to exactly one match.
"""

import argparse
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
HEADER = os.path.join(REPO, "include", "hub", "game_definitions.hpp")
SOURCES = [
    os.path.join(HERE, "sigcheck", "sigcheck.cpp"),
    os.path.join(REPO, "src", "common", "sigscan.cpp"),
]

# constexpr std::string_view NAME =\n    "AA BB ? ...";
PATTERN_RE = re.compile(
    r'constexpr\s+std::string_view\s+(\w+)\s*=\s*"([0-9A-Fa-f?\s]+)"\s*;', re.S
)


def read_signatures():
    source = open(HEADER, encoding="utf-8").read()
    start = source.index("namespace signatures")
    end = source.index("} // namespace signatures", start)
    return PATTERN_RE.findall(source[start:end])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--game-dir",
        default=os.environ.get("FFXIV_HUB_GAME_DIR", os.path.expanduser("~/ffxiv/game")),
        help="directory holding ffxiv_dx11.exe (default: $FFXIV_HUB_GAME_DIR or ~/ffxiv/game)",
    )
    parser.add_argument("--cxx", default=os.environ.get("CXX", "c++"))
    args = parser.parse_args()

    exe = os.path.join(args.game_dir, "ffxiv_dx11.exe")
    if not os.path.exists(exe):
        sys.exit(f"no ffxiv_dx11.exe under {args.game_dir}")

    signatures = read_signatures()
    if not signatures:
        sys.exit(f"no signatures found in {HEADER}")

    binary = os.path.join(
        os.environ.get("TMPDIR", "/tmp"), "ffxiv_hub_sigcheck"
    )
    build = [args.cxx, "-std=c++20", "-O2", f"-I{REPO}/include", f"-I{REPO}/src",
             *SOURCES, "-o", binary]
    if subprocess.call(build) != 0:
        sys.exit("failed to build the signature checker")

    print(f"checking {len(signatures)} signatures against {exe}\n")
    return subprocess.call([binary, exe] + [f"{n}={p}" for n, p in signatures])


if __name__ == "__main__":
    sys.exit(main())
