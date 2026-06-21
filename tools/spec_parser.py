#!/usr/bin/env python3
"""Parse Wave Race 64 makerom spec files into structured segment metadata."""

from __future__ import annotations

import argparse
import json
import re
from dataclasses import dataclass, field
from pathlib import Path


@dataclass
class Segment:
    name: str
    flags: list[str] = field(default_factory=list)
    includes: list[str] = field(default_factory=list)
    entry: str | None = None
    stack: str | None = None
    address: str | None = None
    after: str | None = None
    number: int | None = None
    raw_lines: list[str] = field(default_factory=list)

    @property
    def is_boot(self) -> bool:
        return "BOOT" in self.flags

    @property
    def is_object(self) -> bool:
        return "OBJECT" in self.flags

    @property
    def is_code(self) -> bool:
        if self.is_boot:
            return True
        if self.name in {"loadCodeSEG"}:
            return True
        if self.name.startswith("prog"):
            return True
        return False

    @property
    def is_overlay(self) -> bool:
        return self.is_code and not self.is_boot


SEGMENT_ID_MAP = {
    "STATIC_SEGMENT": 1,
    "AUTO_SEGMENT": 2,
    "DYNAMIC_SEGMENT": 3,
    "SCREEN_SEGMENT": 4,
    "ENDYNAMIC_SEGMENT": 5,
    "SOT_DYNAMIC_SEGMENT": 6,
    "KN_DYNAMIC_SEGMENT": 7,
    "BANK_SEGMENT": 8,
    "TEX_SEGMENT": 9,
    "TEX2_SEGMENT": 10,
    "TEX3_SEGMENT": 11,
    "TEX4_SEGMENT": 12,
    "COURSE_SEGMENT": 13,
    "COURSE_COM_SEGMENT": 14,
}


def parse_memory_constants(memory_h: Path) -> dict[str, int]:
    text = memory_h.read_text(encoding="utf-8", errors="replace")
    constants: dict[str, int] = {}
    for match in re.finditer(r"#define\s+([A-Za-z0-9_]+)\s+(0x[0-9A-Fa-f]+|\d+)", text):
        name, value = match.group(1), match.group(2)
        constants[name] = int(value, 0)
    return constants


def parse_spec(text: str) -> list[Segment]:
    segments: list[Segment] = []
    current: Segment | None = None

    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("#") or line.startswith("/*") or line.startswith("*"):
            continue
        if line.startswith("#include") or line.startswith("#if") or line.startswith("#elif") or line.startswith("#else") or line.startswith("#endif"):
            continue

        if line.startswith("beginseg"):
            current = Segment(name="")
            continue

        if line.startswith("endseg"):
            if current and current.name:
                segments.append(current)
            current = None
            continue

        if current is None:
            continue

        current.raw_lines.append(line)

        if line.startswith("name "):
            current.name = line.split('"')[1]
        elif line.startswith("flags "):
            current.flags = line.replace("flags", "", 1).strip().split()
        elif line.startswith("entry "):
            current.entry = line.split(None, 1)[1].strip()
        elif line.startswith("stack "):
            current.stack = line.split(None, 1)[1].strip()
        elif line.startswith("address "):
            current.address = line.split(None, 1)[1].strip()
        elif line.startswith("after "):
            current.after = line.split(None, 1)[1].strip()
        elif line.startswith("number "):
            token = line.split(None, 1)[1].strip()
            if token.isdigit():
                current.number = int(token)
            else:
                current.address = token  # e.g. number STATIC_SEGMENT
        elif line.startswith("include "):
            include = line.split('"')[1]
            current.includes.append(include)

    return segments


def resolve_address(segment: Segment, constants: dict[str, int]) -> int | None:
    if not segment.address:
        return None
    token = segment.address.strip()
    if token in constants:
        return constants[token]
    if token.startswith("0x"):
        return int(token, 16)
    return None


def export_segments(segments: list[Segment], constants: dict[str, int]) -> list[dict]:
    out = []
    for seg in segments:
        out.append(
            {
                "name": seg.name,
                "flags": seg.flags,
                "includes": seg.includes,
                "entry": seg.entry,
                "address": seg.address,
                "address_value": resolve_address(seg, constants),
                "after": seg.after,
                "number": seg.number,
                "segment_id": SEGMENT_ID_MAP.get(seg.address or "", seg.number),
                "is_boot": seg.is_boot,
                "is_code": seg.is_code,
                "is_object": seg.is_object,
                "is_overlay": seg.is_overlay,
            }
        )
    return out


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--spec", type=Path, default=Path(__file__).resolve().parent.parent / "leak-ref" / "spec")
    parser.add_argument("--memory", type=Path, default=Path(__file__).resolve().parent.parent / "leak-ref" / "kn_memory.h")
    parser.add_argument("--out-json", type=Path, default=Path(__file__).resolve().parent.parent / "tools" / "wr64_segments.json")
    parser.add_argument("--out-md", type=Path, default=Path(__file__).resolve().parent.parent / "docs" / "OVERLAYS.md")
    args = parser.parse_args()

    segments = parse_spec(args.spec.read_text(encoding="utf-8", errors="replace"))
    constants = parse_memory_constants(args.memory)
    exported = export_segments(segments, constants)

    args.out_json.parent.mkdir(parents=True, exist_ok=True)
    args.out_json.write_text(json.dumps(exported, indent=2), encoding="utf-8")

    lines = [
        "# Wave Race 64 segment map",
        "",
        f"Parsed from `{args.spec}` ({len(segments)} segments).",
        "",
        "| Name | VRAM | Flags | Includes | Overlay |",
        "| --- | --- | --- | --- | --- |",
    ]
    for seg in exported:
        includes = ", ".join(seg["includes"][:3])
        if len(seg["includes"]) > 3:
            includes += f", … (+{len(seg['includes']) - 3})"
        vram = f"0x{seg['address_value']:08X}" if seg["address_value"] is not None else seg["address"] or seg["after"] or "-"
        lines.append(
            f"| {seg['name']} | {vram} | {' '.join(seg['flags']) or '-'} | {includes or '-'} | {'yes' if seg['is_overlay'] else 'no'} |"
        )

    args.out_md.parent.mkdir(parents=True, exist_ok=True)
    args.out_md.write_text("\n".join(lines) + "\n", encoding="utf-8")

    print(f"Parsed {len(segments)} segments -> {args.out_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
