#!/usr/bin/env python3
"""Print ROM fingerprint details for WR64 port validation."""

from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path

from rom_util import load_rom

EXPECTED_SHA1 = "508DFC2D4CAA42B6F6DE5263D0AED5E44AC7966A"
EXPECTED_XXH3 = 0x2B675E2250A604FC


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rom", type=Path, nargs="?", default=Path(__file__).resolve().parent.parent / "wr64.us.revA.z64")
    args = parser.parse_args()

    if not args.rom.is_file():
        print(f"ROM not found: {args.rom}", file=sys.stderr)
        return 1

    raw = args.rom.read_bytes()
    normalized, header = load_rom(args.rom)
    sha1 = hashlib.sha1(normalized).hexdigest().upper()

    print(f"Path: {args.rom}")
    print(f"Raw size: {len(raw)}")
    print(f"Format: {header['source_format']}")
    print(f"Title: {header['title']}")
    print(f"PC: 0x{header['pc']:08X}")
    print(f"SHA-1: {sha1}")
    print(f"Expected: {EXPECTED_SHA1}")
    try:
        import xxhash

        xxh3 = xxhash.xxh3_64(normalized).intdigest()
        print(f"XXH3-64: 0x{xxh3:016X}")
    except ImportError:
        xxh3 = None
    sha_ok = sha1 == EXPECTED_SHA1
    xxh_ok = xxh3 == EXPECTED_XXH3 if xxh3 is not None else True
    print("SHA-1 match:" if sha_ok else "SHA-1 mismatch:", sha_ok)
    if xxh3 is not None:
        print("XXH3 match:" if xxh_ok else "XXH3 mismatch:", xxh_ok)
    return 0 if (sha_ok and xxh_ok) else 2


if __name__ == "__main__":
    raise SystemExit(main())
