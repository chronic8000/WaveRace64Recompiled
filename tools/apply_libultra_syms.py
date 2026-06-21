#!/usr/bin/env python3
"""Apply libultra symbol names to wr64.us.syms.toml from wr64_libultra_map.json."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--map",
        type=Path,
        default=Path(__file__).resolve().parent / "wr64_libultra_map.json",
    )
    parser.add_argument(
        "--syms",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "wr64.us.syms.toml",
    )
    args = parser.parse_args()

    mapping = {
        int(vram, 16): name
        for vram, name in json.loads(args.map.read_text(encoding="utf-8")).items()
        if not vram.startswith("_")
    }

    text = args.syms.read_text(encoding="utf-8")
    replaced = 0
    for vram, name in sorted(mapping.items()):
        vram_hex = f"0x{vram:08x}"
        pattern = re.compile(
            rf'(\{{ name = )"[^"]+"(, vram = {re.escape(vram_hex)}, size = \d+ \}})'
        )
        new_text, count = pattern.subn(rf'\1"{name}"\2', text, count=1)
        if count == 0:
            print(f"warning: no symbol at {vram_hex}")
        else:
            replaced += count
            text = new_text

    args.syms.write_text(text, encoding="utf-8", newline="\n")
    print(f"Renamed {replaced} libultra symbols in {args.syms}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
