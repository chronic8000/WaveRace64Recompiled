#!/usr/bin/env python3
"""Extract WR64 audio cart blobs from the user ROM using spec layout."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from rom_util import load_rom


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom", type=Path, default=Path(__file__).resolve().parent.parent / "wr64.us.revA.z64")
    parser.add_argument("--layout", type=Path, default=Path(__file__).resolve().parent / "wr64_rom_layout.json")
    parser.add_argument("--out-dir", type=Path, default=Path(__file__).resolve().parent.parent / "assets" / "runtime")
    args = parser.parse_args()

    rom, _ = load_rom(args.rom)
    layout = json.loads(args.layout.read_text(encoding="utf-8"))

    audio_names = {"Audiobank", "Audiotable", "Audioseq", "audio/aspMain.o"}
    args.out_dir.mkdir(parents=True, exist_ok=True)

    extracted = 0
    for seg in layout["segments"]:
        if seg["name"] not in audio_names and not seg["name"].startswith("audio"):
            continue
        blob = rom[seg["rom_start"] : seg["rom_end"]]
        out = args.out_dir / f"{seg['name'].replace('/', '_')}.bin"
        out.write_bytes(blob)
        print(f"Wrote {out} ({len(blob)} bytes)")
        extracted += 1

    if extracted == 0:
        print("No audio segments matched; check wr64_rom_layout.json names.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
