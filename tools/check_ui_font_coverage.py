#!/usr/bin/env python3
"""Fail the build when runtime UI text is absent from the shared UI fonts."""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE_SUFFIXES = {".c", ".h", ".cpp", ".hpp"}


def required_codepoints() -> set[int]:
    required: set[int] = set()
    quoted = re.compile(r'"(?:\\.|[^"\\])*"')
    for path in sorted((ROOT / "main").iterdir()):
        if path.suffix not in SOURCE_SUFFIXES or path.name.startswith("ui_font_source_han_"):
            continue
        for literal in quoted.findall(path.read_text(encoding="utf-8")):
            for char in literal:
                cp = ord(char)
                # Match the generator: all printable ASCII plus every
                # printable Unicode character found in a runtime literal.
                # This prevents punctuation outside the CJK block from being
                # silently omitted while coverage still reports success.
                if char.isprintable() and (0x20 <= cp <= 0x7E or cp >= 0x80):
                    required.add(cp)
    required.add(0x25A1)
    return required


def embedded_codepoints(size: int) -> set[int]:
    source = (ROOT / "main" / f"ui_font_source_han_{size}.c").read_text(encoding="utf-8")
    return {int(match, 16) for match in re.findall(r"\{0x([0-9a-fA-F]+),", source)}


def main() -> int:
    required = required_codepoints()
    failed = False
    for size in (20, 24):
        missing = required - embedded_codepoints(size)
        if missing:
            failed = True
            text = "".join(chr(cp) for cp in sorted(missing))
            print(f"ERROR: Source Han Sans {size}px subset is missing: {text}", file=sys.stderr)
    if failed:
        print("Run tools/generate_ui_font.py for both --size 20 and --size 24.", file=sys.stderr)
        return 1
    print(f"UI font coverage verified: {len(required)} runtime codepoints in 20px and 24px")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
