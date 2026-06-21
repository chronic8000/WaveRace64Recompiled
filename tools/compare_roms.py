#!/usr/bin/env python3
"""Compare WR64 ROM dumps."""

from __future__ import annotations

import hashlib
import sys
from pathlib import Path

from rom_util import load_rom

try:
    import xxhash
except ImportError:
    xxhash = None


def fingerprint(path: Path) -> dict:
    raw = path.read_bytes()
    norm, hdr = load_rom(path)
    sha1 = hashlib.sha1(norm).hexdigest().upper()
    xxh = xxhash.xxh3_64(norm).intdigest() if xxhash else None
    return {
        "path": str(path),
        "raw_size": len(raw),
        "format": hdr["source_format"],
        "title": hdr["title"],
        "pc": hdr["pc"],
        "sha1": sha1,
        "xxh3": xxh,
        "normalized": norm,
    }


def main() -> int:
    paths = [Path(p) for p in sys.argv[1:]]
    results = []
    for p in paths:
        if not p.is_file():
            print(f"MISSING: {p}")
            continue
        results.append(fingerprint(p))

    for r in results:
        print("---")
        print(f"File: {r['path']}")
        print(f"  Raw size: {r['raw_size']}")
        print(f"  Format: {r['format']}")
        print(f"  Title: {r['title']}")
        print(f"  PC: 0x{r['pc']:08X}")
        print(f"  SHA-1: {r['sha1']}")
        if r["xxh3"] is not None:
            print(f"  XXH3-64: 0x{r['xxh3']:016X}")

    if len(results) >= 2:
        a, b = results[0], results[1]
        same = a["normalized"] == b["normalized"]
        print("\n=== Comparison (first two files) ===")
        print(f"Identical after normalization: {same}")
        if not same:
            diffs = sum(1 for x, y in zip(a["normalized"], b["normalized"]) if x != y)
            diffs += abs(len(a["normalized"]) - len(b["normalized"]))
            print(f"Byte differences (approx): {diffs}")

    if results:
        print("\n=== Port config (supported_games) ===")
        print("Expected XXH3-64: 0x2B675E2250A604FC")
        for r in results:
            match = r["xxh3"] == 0x2B675E2250A604FC if r["xxh3"] else False
            print(f"  {Path(r['path']).name}: {'MATCH' if match else 'NO MATCH'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
