#!/usr/bin/env python3
"""Validation matrix helper for Wave Race 64 Recompiled."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


MODES = [
    "Boot / attract",
    "Title menu",
    "Time trial course load",
    "VS race",
    "Championship",
    "2P split-screen",
    "Ghost / Controller Pak",
    "Options / erase / name entry",
]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()

    checks = []

    rom = args.project / "wr64.us.revA.z64"
    if rom.is_file():
        checks.insert(0, ("ROM fingerprint", [sys.executable, str(args.project / "tools" / "rom_fingerprint.py"), str(rom)]))
    else:
        print("[SKIP] ROM fingerprint (no wr64.us.revA.z64 in tree)")

    syms = args.project / "wr64.us.syms.toml"
    checks.append(("Symbols TOML", [sys.executable, "-c", f"import pathlib; pathlib.Path('{syms.as_posix()}').is_file() or exit(1)"]))

    recomp = args.project / "RecompiledFuncs" / "funcs.h"
    checks.append(("Recompiled funcs", [sys.executable, "-c", f"import pathlib; pathlib.Path('{recomp.as_posix()}').is_file() or exit(1)"]))

    lookup = args.project / "RecompiledFuncs" / "lookup.cpp"
    checks.append(("Lookup table", [sys.executable, "-c", f"import pathlib; pathlib.Path('{lookup.as_posix()}').is_file() or exit(1)"]))

    overlays = args.project / "RecompiledFuncs" / "recomp_overlays.inl"
    checks.append(("Overlay table", [sys.executable, "-c", f"import pathlib; pathlib.Path('{overlays.as_posix()}').is_file() or exit(1)"]))

    failed = 0
    for name, cmd in checks:
        if cmd[1].endswith(".py") or cmd[1] == "-c":
            proc = subprocess.run(cmd, cwd=args.project, capture_output=True, text=True)
        else:
            proc = subprocess.run(cmd, cwd=args.project, capture_output=True, text=True)
        ok = proc.returncode == 0
        print(f"[{'PASS' if ok else 'FAIL'}] {name}")
        if not ok and proc.stderr:
            print(proc.stderr.strip())
        failed += 0 if ok else 1

    print("\nManual playtest matrix (compare vs emulator on locked ROM hash):")
    for mode in MODES:
        print(f"  - [ ] {mode}")

    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
