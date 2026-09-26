#!/usr/bin/env python3
"""Fails when a source includes a header from a layer it must not depend on.

The layers are AGENTS.md's: plugins depend on common, the payload and the app
depend on the plugins and common, and nothing depends on the payload or the app.
The CMake targets link the same way, but `-Isrc` exposes `payload/` and `app/`
to every layer, so an include is only rejected here.

Run from anywhere; exits 1 and lists each violation.
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent

# Folder -> include prefixes its sources must not use.
RULES = {
    "include/hub": ("meter/", "mitigator/", "payload/", "app/"),
    "src/common": ("meter/", "mitigator/", "payload/", "app/"),
    "plugins/combat_meter": ("mitigator/", "payload/", "app/"),
    "plugins/latency_mitigator": ("meter/", "payload/", "app/"),
    "src/payload": ("app/",),
    "src/app": ("payload/",),
}

SOURCE_SUFFIXES = {".cpp", ".hpp", ".h", ".inl", ".rc"}
INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"')


def main() -> int:
    violations = []
    for folder, forbidden in RULES.items():
        for path in sorted((ROOT / folder).rglob("*")):
            if path.suffix not in SOURCE_SUFFIXES or not path.is_file():
                continue
            text = path.read_text(encoding="utf-8", errors="replace")
            for number, line in enumerate(text.splitlines(), start=1):
                match = INCLUDE.match(line)
                if match and match.group(1).startswith(forbidden):
                    rel = path.relative_to(ROOT).as_posix()
                    violations.append(f"{rel}:{number}: includes \"{match.group(1)}\" across a layer")
    for violation in violations:
        print(violation, file=sys.stderr)
    if violations:
        print(f"check_layers: {len(violations)} layering violation(s); see AGENTS.md", file=sys.stderr)
        return 1
    print("Layering check: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
