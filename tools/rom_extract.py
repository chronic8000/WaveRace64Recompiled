#!/usr/bin/env python3
"""Extract sequential makerom segment layout from a WR64 ROM + spec metadata."""

from __future__ import annotations

import argparse
import json
import struct
from dataclasses import dataclass
from pathlib import Path

from rom_util import load_rom
from spec_parser import parse_memory_constants, parse_spec, resolve_address


ALIGN = 16


def align16(value: int) -> int:
    return (value + 15) & ~15


def mio0_size(rom: bytes, offset: int) -> int | None:
    if offset + 16 > len(rom) or rom[offset : offset + 4] != b"MIO0":
        return None
    _, dest_size, comp_offset, raw_offset = struct.unpack(">4I", rom[offset : offset + 16])
    ends = [16]
    if comp_offset:
        ends.append(comp_offset + dest_size)
    if raw_offset:
        ends.append(raw_offset + dest_size)
    size = align16(max(ends))
    if offset + size > len(rom):
        return None
    return size


def guess_plain_size(rom: bytes, offset: int, max_size: int = 0x400000) -> int:
    """Heuristic size for non-MIO0 chunks: stop at long zero padding."""
    limit = min(len(rom), offset + max_size)
    run = 0
    for pos in range(offset, limit, 4):
        word = rom[pos : pos + 4]
        if word in (b"\x00" * 4, b"\xFF" * 4):
            run += 4
            if run >= 64 and (pos - offset) >= 0x100:
                return align16(pos - offset - run + 64)
        else:
            run = 0
    return align16(limit - offset)


@dataclass
class RomSegment:
    name: str
    rom_start: int
    rom_end: int
    vram: int | None
    flags: list[str]
    includes: list[str]
    is_code: bool
    is_overlay: bool

    @property
    def size(self) -> int:
        return self.rom_end - self.rom_start


def build_layout(rom: bytes, spec_path: Path, memory_path: Path) -> list[RomSegment]:
    segments = parse_spec(spec_path.read_text(encoding="utf-8", errors="replace"))
    constants = parse_memory_constants(memory_path)

    cursor = 0x1000
    layout: list[RomSegment] = []
    code_vram_cursor = constants.get("BOOT_ADDR", 0x80046800)

    for seg in segments:
        rom_start = align16(cursor)

        size = mio0_size(rom, rom_start)
        if size is None:
            if seg.is_boot or seg.name == "loadCodeSEG" or seg.name.startswith("prog"):
                size = guess_plain_size(rom, rom_start, max_size=0x200000 if seg.is_boot else 0x100000)
            else:
                size = guess_plain_size(rom, rom_start, max_size=0x800000)

        rom_end = rom_start + size
        if rom_end > len(rom):
            rom_end = len(rom)
            size = rom_end - rom_start

        vram = resolve_address(seg, constants)
        if seg.is_boot:
            vram = constants.get("BOOT_ADDR", 0x80046800)
        elif seg.name == "loadCodeSEG":
            vram = code_vram_cursor
        elif seg.is_code and vram is None:
            vram = constants.get("PROG_BANK")

        if seg.is_boot:
            code_vram_cursor = (vram or 0) + size
        elif seg.name == "loadCodeSEG" and vram is not None:
            code_vram_cursor = vram + size

        layout.append(
            RomSegment(
                name=seg.name,
                rom_start=rom_start,
                rom_end=rom_end,
                vram=vram,
                flags=list(seg.flags),
                includes=list(seg.includes),
                is_code=seg.is_code,
                is_overlay=seg.is_overlay,
            )
        )
        cursor = rom_end

    return layout


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom", type=Path, default=Path(__file__).resolve().parent.parent / "wr64.us.revA.z64")
    parser.add_argument("--spec", type=Path, default=Path(__file__).resolve().parent.parent / "leak-ref" / "spec")
    parser.add_argument("--memory", type=Path, default=Path(__file__).resolve().parent.parent / "leak-ref" / "kn_memory.h")
    parser.add_argument("--out", type=Path, default=Path(__file__).resolve().parent.parent / "tools" / "wr64_rom_layout.json")
    args = parser.parse_args()

    rom, header = load_rom(args.rom)
    layout = build_layout(rom, args.spec, args.memory)

    payload = {
        "rom_header": header,
        "segments": [
            {
                "name": seg.name,
                "rom_start": seg.rom_start,
                "rom_end": seg.rom_end,
                "size": seg.size,
                "vram": seg.vram,
                "flags": seg.flags,
                "includes": seg.includes,
                "is_code": seg.is_code,
                "is_overlay": seg.is_overlay,
            }
            for seg in layout
        ],
    }

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    print(f"Extracted {len(layout)} ROM segments -> {args.out}")
    print(f"ROM title: {header['title']} PC=0x{header['pc']:08X}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
