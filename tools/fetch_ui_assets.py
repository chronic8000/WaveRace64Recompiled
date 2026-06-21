#!/usr/bin/env python3
"""Ensure launcher UI fonts exist in assets/ (tracked in git, same as Zelda64Recomp)."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets"

FONT_PATHS = [
    ASSETS / "ChiaroBold.otf",
    ASSETS / "ChiaroNormal.otf",
    ASSETS / "LatoLatin-Bold.ttf",
    ASSETS / "LatoLatin-BoldItalic.ttf",
    ASSETS / "LatoLatin-Italic.ttf",
    ASSETS / "LatoLatin-Regular.ttf",
    ASSETS / "NotoEmoji-Regular.ttf",
    ASSETS / "promptfont" / "promptfont.ttf",
]


def main() -> int:
    missing = [p for p in FONT_PATHS if not p.exists()]
    if not missing:
        print("UI fonts already present in assets/")
        return 0

    print("Restoring missing UI fonts from git...")
    rel_paths = [str(p.relative_to(ROOT)).replace("\\", "/") for p in missing]
    result = subprocess.run(
        ["git", "checkout", "HEAD", "--", *rel_paths],
        cwd=ROOT,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        print(result.stderr or result.stdout, file=sys.stderr)
        return result.returncode

    still_missing = [p for p in FONT_PATHS if not p.exists()]
    if still_missing:
        print("Still missing fonts:", ", ".join(p.name for p in still_missing), file=sys.stderr)
        return 1

    print("UI fonts restored.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
