#!/usr/bin/env python3
"""Generate N64Recomp symbols TOML from ROM layout + MIPS function scanning."""

from __future__ import annotations

import argparse
import json
import re
import struct
from dataclasses import dataclass
from pathlib import Path

import rabbitizer

from rom_util import load_rom


@dataclass
class Function:
    name: str
    vram: int
    size: int


def is_mips_return(ins: rabbitizer.Instruction, word: int) -> bool:
    """Detect function epilogue (jr $ra / return) robustly across rabbitizer versions."""
    if not ins.isValid():
        return False
    if ins.isReturn():
        return True
    if ins.isJrRa():
        return True
    # Fallback: explicit jr $ra encoding (0x03E00008) when register enums fail comparisons.
    try:
        if ins.getOpcodeName() == "jr":
            rs = ins.getRs()
            ra = rabbitizer.RegGprO32.ra
            if rs == ra or int(rs) == int(ra):
                return True
    except (AttributeError, TypeError, ValueError):
        pass
    return (word & 0xFC1FFFFF) == 0x00000008


def scan_functions(rom: bytes, rom_start: int, rom_end: int, vram_base_addr: int, prefix: str) -> list[Function]:
    starts = {rom_start}
    for off in range(rom_start, rom_end - 4, 4):
        word = struct.unpack(">I", rom[off : off + 4])[0]
        ins = rabbitizer.Instruction(word)
        if not ins.isValid():
            continue
        op = ins.getOpcodeName()
        if op == "jal":
            pc = vram_base_addr + (off - rom_start)
            ins.vram = pc
            target_vram = ins.getBranchVramGeneric()
            rom_target = rom_start + (target_vram - vram_base_addr)
            if rom_start <= rom_target < rom_end:
                starts.add(rom_target)
        elif is_mips_return(ins, word):
            next_off = align(off + 8, 4)
            if next_off < rom_end:
                starts.add(next_off)

    offsets = sorted(starts)
    functions: list[Function] = []
    for idx, start in enumerate(offsets):
        end = offsets[idx + 1] if idx + 1 < len(offsets) else rom_end
        size = align(end - start, 4)
        vram = vram_base_addr + (start - rom_start)
        if size < 16:
            if functions:
                prev = functions[-1]
                functions[-1] = Function(prev.name, prev.vram, prev.size + size)
            continue
        functions.append(Function(name=f"{prefix}_{vram:08X}", vram=vram, size=size))

    if not functions and rom_end > rom_start:
        functions.append(
            Function(
                name=f"{prefix}_{vram_base_addr:08X}",
                vram=vram_base_addr,
                size=align(rom_end - rom_start, 4),
            )
        )
    return functions


def function_branch_targets(rom: bytes, fn: Function, rom_start: int, vram_base: int) -> set[int]:
    targets: set[int] = set()
    fn_rom_start = rom_start + (fn.vram - vram_base)
    for off in range(fn_rom_start, fn_rom_start + fn.size, 4):
        if off + 4 > len(rom):
            break
        word = struct.unpack(">I", rom[off : off + 4])[0]
        ins = rabbitizer.Instruction(word)
        if not ins.isValid():
            continue
        pc = fn.vram + (off - fn_rom_start)
        ins.vram = pc
        op = ins.getOpcodeName()
        if ins.isBranch() or op == "j":
            targets.add(int(ins.getBranchVramGeneric()))
    return targets


def merge_functions_for_inward_branches(
    functions: list[Function], rom: bytes, rom_start: int, vram_base: int
) -> list[Function]:
    """Merge scanned functions when branches target another function's interior."""
    if len(functions) <= 1:
        return functions

    while True:
        parent = list(range(len(functions)))

        def find(i: int) -> int:
            while parent[i] != i:
                parent[i] = parent[parent[i]]
                i = parent[i]
            return i

        def union(i: int, j: int) -> None:
            ri, rj = find(i), find(j)
            if ri != rj:
                parent[rj] = ri

        def index_for_vram(vram: int) -> int | None:
            for idx, fn in enumerate(functions):
                if fn.vram <= vram < fn.vram + fn.size:
                    return idx
            return None

        for idx, fn in enumerate(functions):
            for target in function_branch_targets(rom, fn, rom_start, vram_base):
                target_idx = index_for_vram(target)
                if target_idx is None or target_idx == idx:
                    continue
                union(idx, target_idx)

        groups: dict[int, list[int]] = {}
        for idx in range(len(functions)):
            groups.setdefault(find(idx), []).append(idx)

        if len(groups) == len(functions):
            break

        merged: list[Function] = []
        for root in sorted(groups, key=lambda r: functions[groups[r][0]].vram):
            indices = sorted(groups[root], key=lambda i: functions[i].vram)
            first = functions[indices[0]]
            last = functions[indices[-1]]
            merged.append(
                Function(
                    name=first.name,
                    vram=first.vram,
                    size=last.vram + last.size - first.vram,
                )
            )
        functions = merged

    return functions


def align(value: int, alignment: int) -> int:
    return (value + alignment - 1) // alignment * alignment


def parse_header_symbols(leak_ref: Path) -> dict[int, str]:
    """Collect function names from leak headers/surviving C sources."""
    names: dict[int, str] = {}
    proto_re = re.compile(r"^\s*(?:extern\s+)?(?:void|int|s32|u32|u16|s16|float|double|char|short|long|Gfx\s*\*|OSMesg|OSIoMesg|OSThread)\s+(?:/\*.*?\*/\s*)?([A-Za-z_][A-Za-z0-9_]*)\s*\(", re.MULTILINE)

    for path in leak_ref.rglob("*"):
        if path.suffix not in {".h", ".c"}:
            continue
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for match in proto_re.finditer(text):
            names.setdefault(match.group(1), match.group(1))
    return {hash(name) & 0xFFFFFFFF: name for name in names.values()}


# Boot PI DMA slices observed in wr64_boot.log (MIPS at dma1; dma2 is mostly Gfx DL).
BOOT_DMA_SLICES = [
    {
        "name": "..ovl_boot_dma1",
        "rom_start": 0xA95D0,
        "size": 0x4CAC0,
        "vram": 0x801DAFA0,
        "split": True,
        "merge_inward": False,
    },
    # DMA #2 (ROM 0xF6090) is Gfx display-list data, not MIPS — raw DMA only.
]

# Prog overlays from refs/Wave-Race-64/src/ovl_table.c — all load to VRAM 0x802C5800.
# Export anchors are entry points called from codeSEG (func_80092CF0 / func_800922E4).
PROG_OVERLAY_EXPORT_VRAMS: dict[str, list[int]] = {
    "segment_1B1FB0": [
        0x802C5800,
        0x802C583C,
        0x802C5BA4,
        0x802C7090,
        0x802C7304,
        0x802C73B0,
        0x802C7484,
        0x802C7510,
        0x802C7578,
        0x802C7608,
    ],
    "ovl_i0": [0x802C5F6C],
    "ovl_i1": [0x802C5968],
    "ovl_i2": [0x802C5AE4],
    "ovl_i3": [0x802C5800, 0x802C5F50],
    "ovl_i4": [0x802C5A7C],
    "ovl_i5": [0x802C6944, 0x802C7090],
    "ovl_i6": [0x802C7D00],
    "seg_1C3780": [0x802C5800],
    "seg_1C3D00": [0x802C5800],
    "ovl_i7": [0x802C913C],
    "ovl_i8": [0x802C5B74],
    "ovl_i9": [0x802C5924],
    "ovl_i10": [0x802C5B4C],
    "ovl_i11": [0x802C5B78],
    "ovl_i12": [0x802C5B40],
    "ovl_i13": [0x802C5C1C],
    "ovl_i14": [0x802C5D3C],
    "ovl_i15": [0x802C5D24],
}

PROG_OVERLAY_SLICES = [
    {"name": "..ovl_segment_1B1FB0", "rom_start": 0x1B1FB0, "size": 0x1B3EC0 - 0x1B1FB0, "vram": 0x802C5800},
    {"name": "..ovl_ovl_i0", "rom_start": 0x1B3EC0, "size": 0x1B55A0 - 0x1B3EC0, "vram": 0x802C5800},
    {"name": "..ovl_ovl_i1", "rom_start": 0x1B55A0, "size": 0x1B9440 - 0x1B55A0, "vram": 0x802C5800},
    {"name": "..ovl_ovl_i2", "rom_start": 0x1B9440, "size": 0x1BC890 - 0x1B9440, "vram": 0x802C5800},
    {"name": "..ovl_ovl_i3", "rom_start": 0x1BC890, "size": 0x1BE0B0 - 0x1BC890, "vram": 0x802C5800},
    {"name": "..ovl_ovl_i4", "rom_start": 0x1BE0B0, "size": 0x1BFF50 - 0x1BE0B0, "vram": 0x802C5800},
    {"name": "..ovl_ovl_i5", "rom_start": 0x1BFF50, "size": 0x1C2250 - 0x1BFF50, "vram": 0x802C5800},
    {"name": "..ovl_ovl_i6", "rom_start": 0x1C2250, "size": 0x1C3780 - 0x1C2250, "vram": 0x802C5800},
    {"name": "..ovl_seg_1C3780", "rom_start": 0x1C3780, "size": 0x1C3D00 - 0x1C3780, "vram": 0x802C5800},
    {"name": "..ovl_seg_1C3D00", "rom_start": 0x1C3D00, "size": 0x1C43F0 - 0x1C3D00, "vram": 0x802C5800},
    {"name": "..ovl_ovl_i7", "rom_start": 0x1C43F0, "size": 0x1C49A0 - 0x1C43F0, "vram": 0x802C5800},
    {"name": "..ovl_ovl_i8", "rom_start": 0x1C49A0, "size": 0x1C66D0 - 0x1C49A0, "vram": 0x802C5800},
    {"name": "..ovl_ovl_i9", "rom_start": 0x1C66D0, "size": 0x1C9150 - 0x1C66D0, "vram": 0x802C5800},
    {"name": "..ovl_ovl_i10", "rom_start": 0x1C9150, "size": 0x1CA480 - 0x1C9150, "vram": 0x802C5800},
    {"name": "..ovl_ovl_i11", "rom_start": 0x1CA480, "size": 0x1CAE40 - 0x1CA480, "vram": 0x802C5800},
    {"name": "..ovl_ovl_i12", "rom_start": 0x1CAE40, "size": 0x1CBAF0 - 0x1CAE40, "vram": 0x802C5800},
    {"name": "..ovl_ovl_i13", "rom_start": 0x1CBAF0, "size": 0x1CF180 - 0x1CBAF0, "vram": 0x802C5800},
    {"name": "..ovl_ovl_i14", "rom_start": 0x1CF180, "size": 0x1CFB60 - 0x1CF180, "vram": 0x802C5800},
    {"name": "..ovl_ovl_i15", "rom_start": 0x1CFB60, "size": 0x1D11D0 - 0x1CFB60, "vram": 0x802C5800},
]

# Empty callees in func_80091F50 — explicit symbols so jal targets resolve.
CODESEG_GAP_SYMBOLS = [
    {"name": "codeSEG_80051530", "vram": 0x80051530, "size": 8},
    {"name": "codeSEG_80088EA0", "vram": 0x80088EA0, "size": 8},
]


# Nominal load addresses for PI DMA relocatable blobs (kn_memory.h).
NOMINAL_VRAM = {
    "prog": 0x802C5800,
    "bank": 0x80316800,
    "course": 0x802D6800,
    "course_com": 0x802D6800,
    "course_dt": 0x80306800,
    "start_gate": 0x8029A200,
    "edit": 0x80328000,
}


def nominal_vram_for_segment(name: str, layout_vram: int | None) -> int:
    if layout_vram is not None and layout_vram >= 0x80000000:
        return layout_vram
    if name == "loadCodeSEG":
        return NOMINAL_VRAM["loadCodeSEG"]
    if name.endswith("_com"):
        return NOMINAL_VRAM["course_com"]
    if "DT" in name or name.endswith("DT"):
        return NOMINAL_VRAM["course_dt"]
    if name.startswith("course"):
        return NOMINAL_VRAM["course"]
    if name.startswith("bank"):
        return NOMINAL_VRAM["bank"]
    if name.startswith("prog"):
        return NOMINAL_VRAM["prog"]
    if name.startswith("start_gate"):
        return NOMINAL_VRAM["start_gate"]
    if name.startswith("edit"):
        return NOMINAL_VRAM["edit"]
    return 0x80200000


def is_dma_relocatable_segment(seg: dict) -> bool:
    name = seg["name"]
    size = seg.get("size", 0)
    if size < 256:
        return False
    if name == "loadCodeSEG":
        return True
    # prog*.o entries in spec are linker stubs; gameplay code is in bank*/course* MIO0.
    if name.startswith("prog"):
        return False
    prefixes = (
        "course",
        "bank",
        "start_gate",
        "jet",
        "edit",
        "tex",
    )
    return name.startswith(prefixes)


def should_split_functions(seg: dict, split_functions: bool) -> bool:
    if not split_functions:
        return False
    name = seg["name"]
    # Course/bank blobs are mostly data; per-function scan can blow up N64Recomp.
    if name.startswith(("course", "bank", "start_gate", "jet", "tex", "edit")):
        return False
    return True


def build_export_anchor_functions(slice_def: dict, export_vrams: list[int], prefix: str) -> list[Function]:
    """One recompiled function per export anchor; spans until the next anchor or section end."""
    vram_base = slice_def["vram"]
    section_end = vram_base + slice_def["size"]
    starts = sorted({v for v in export_vrams if vram_base <= v < section_end})
    if not starts:
        starts = [vram_base]

    functions: list[Function] = []
    for idx, vram in enumerate(starts):
        end_vram = starts[idx + 1] if idx + 1 < len(starts) else section_end
        size = align(end_vram - vram, 4)
        if size < 4:
            continue
        functions.append(Function(name=f"{prefix}_{vram:08X}", vram=vram, size=size))
    return functions


def build_dma_slice_section(
    slice_def: dict, rom: bytes, split_functions: bool, *, merge_inward_branches: bool
) -> dict:
    rom_start = slice_def["rom_start"]
    rom_end = rom_start + slice_def["size"]
    vram = slice_def["vram"]
    prefix = slice_def["name"].removeprefix("..ovl_")
    export_vrams = slice_def.get("exports") or PROG_OVERLAY_EXPORT_VRAMS.get(prefix)
    do_split = split_functions and slice_def.get("split", True)
    slice_merge = slice_def.get("merge_inward", merge_inward_branches)

    if export_vrams is not None:
        functions = build_export_anchor_functions(slice_def, export_vrams, prefix)
    elif do_split:
        functions = scan_functions(rom, rom_start, rom_end, vram, prefix)
        if slice_merge:
            functions = merge_functions_for_inward_branches(functions, rom, rom_start, vram)
    else:
        functions = [Function(name=prefix, vram=vram, size=slice_def["size"])]

    if not functions:
        functions = [Function(name=prefix, vram=vram, size=slice_def["size"])]

    return {
        "name": slice_def["name"],
        "rom": rom_start,
        "vram": vram,
        "size": slice_def["size"],
        "relocatable": True,
        "functions": [{"name": f.name, "vram": f.vram, "size": f.size} for f in functions],
    }


def build_section(
    seg: dict,
    rom: bytes,
    split_functions: bool,
    *,
    boot_merged: bool = False,
    merge_inward_branches: bool = True,
) -> dict | None:
    vram = nominal_vram_for_segment(seg["name"], seg.get("vram"))
    prefix = seg["name"]
    do_split = should_split_functions(seg, split_functions)

    if do_split:
        functions = scan_functions(rom, seg["rom_start"], seg["rom_end"], vram, prefix)
        if merge_inward_branches:
            functions = merge_functions_for_inward_branches(functions, rom, seg["rom_start"], vram)
        if boot_merged:
            entry_vram = 0x80047AA4
            for fn in functions:
                if fn.vram == entry_vram:
                    fn.name = "BootProcess"
                    break
            else:
                entry_rom = seg["rom_start"] + (entry_vram - vram)
                entry_end = seg["rom_end"]
                for fn in functions:
                    if fn.vram > entry_vram:
                        entry_end = seg["rom_start"] + (fn.vram - vram)
                        break
                functions.insert(
                    0,
                    Function(
                        name="BootProcess",
                        vram=entry_vram,
                        size=align(entry_end - entry_rom, 4),
                    ),
                )
    elif seg["name"] == "codeSEG":
        functions = [Function(name="BootProcess", vram=0x80047AA4, size=seg["size"] - (0x80047AA4 - vram))]
    else:
        functions = [Function(name=prefix, vram=vram, size=seg["size"])]

    if not functions:
        return None

    relocatable = bool(seg.get("is_overlay")) or is_dma_relocatable_segment(seg)
    section_name = seg["name"]
    if relocatable and not section_name.startswith("..ovl_"):
        section_name = f"..ovl_{section_name}"

    return {
        "name": section_name,
        "rom": seg["rom_start"],
        "vram": vram,
        "size": seg["size"],
        "relocatable": relocatable,
        "functions": [{"name": f.name, "vram": f.vram, "size": f.size} for f in functions],
    }


def write_syms_toml(path: Path, sections: list[dict]) -> None:
    lines: list[str] = []
    lines.append("# Auto-generated Wave Race 64 USA symbol map")
    lines.append("")

    for section in sections:
        lines.append("[[section]]")
        lines.append(f'name = "{section["name"]}"')
        lines.append(f"rom = {section['rom']}")
        lines.append(f"vram = {hex(section['vram'])}")
        lines.append(f"size = {section['size']}")
        if section.get("relocatable"):
            lines.append("# relocatable overlay")
        lines.append("functions = [")
        for fn in section["functions"]:
            lines.append(
                f'  {{ name = "{fn["name"]}", vram = {hex(fn["vram"])}, size = {fn["size"]} }},'
            )
        lines.append("]")
        lines.append("")

    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rom", type=Path, default=Path(__file__).resolve().parent.parent / "wr64.us.revA.z64")
    parser.add_argument("--layout", type=Path, default=Path(__file__).resolve().parent.parent / "tools" / "wr64_rom_layout.json")
    parser.add_argument("--out", type=Path, default=Path(__file__).resolve().parent.parent / "wr64.us.syms.toml")
    parser.add_argument("--overlays-out", type=Path, default=Path(__file__).resolve().parent.parent / "overlays.us.txt")
    parser.add_argument("--max-code-sections", type=int, default=256, help="Limit boot code sections")
    parser.add_argument("--max-overlay-sections", type=int, default=256, help="Limit PI DMA overlay sections")
    parser.add_argument(
        "--boot-overlays-only",
        action="store_true",
        help="Emit boot PI DMA overlay slices (not full course MIO0 blobs)",
    )
    parser.add_argument(
        "--boot-dma-slices",
        action="store_true",
        help="Alias for --boot-overlays-only",
    )
    parser.add_argument(
        "--overlays-list-only",
        action="store_true",
        help="Only regenerate overlays.us.txt (no syms.toml changes)",
    )
    parser.add_argument("--split-functions", action="store_true", help="Scan for individual functions instead of one per section")
    parser.add_argument(
        "--no-merge-inward-branches",
        action="store_true",
        help="Do not merge functions when branches target another function's interior (avoids stack frame corruption)",
    )
    parser.add_argument(
        "--prog-overlays",
        action="store_true",
        help="Include 19 prog overlay MIPS slices (segment_1B1FB0 + ovl_i*) at VRAM 0x802C5800",
    )
    parser.add_argument(
        "--layout-overlays",
        action="store_true",
        help="Include bank/course/start_gate overlays from wr64_rom_layout.json in syms + overlays list",
    )
    args = parser.parse_args()
    merge_inward = not args.no_merge_inward_branches

    rom, _ = load_rom(args.rom)
    layout = json.loads(args.layout.read_text(encoding="utf-8"))

    if args.overlays_list_only:
        overlay_names: list[str] = []
        if args.boot_overlays_only or args.boot_dma_slices:
            overlay_names.extend(slice_def["name"] for slice_def in BOOT_DMA_SLICES)
        if args.layout_overlays:
            overlay_names.extend(
                f"..ovl_{seg['name']}"
                for seg in layout["segments"]
                if is_dma_relocatable_segment(seg) and seg["name"] != "codeSEG"
            )
        if args.prog_overlays:
            overlay_names.extend(slice_def["name"] for slice_def in PROG_OVERLAY_SLICES)
        if not overlay_names:
            overlay_names = [
                f"..ovl_{seg['name']}"
                for seg in layout["segments"]
                if is_dma_relocatable_segment(seg) and seg["name"] != "codeSEG"
            ]
        args.overlays_out.write_text("\n".join(overlay_names) + ("\n" if overlay_names else ""), encoding="utf-8")
        print(f"Overlay list ({len(overlay_names)} sections) -> {args.overlays_out}")
        return 0

    sections_out: list[dict] = []
    overlay_names: list[str] = []
    seen_section_names: set[str] = set()

    def append_section(section: dict | None) -> None:
        if section is None or section["name"] in seen_section_names:
            return
        seen_section_names.add(section["name"])
        sections_out.append(section)
        if section.get("relocatable"):
            overlay_names.append(section["name"])

    all_segments = layout["segments"]
    load_seg = next((s for s in all_segments if s["name"] == "loadCodeSEG"), None)
    code_sections = [s for s in all_segments if s["is_code"] and s.get("vram") is not None]

    # Static boot segment: codeSEG ROM includes loadCodeSEG for in-place libultra helpers.
    for seg in code_sections[: args.max_code_sections]:
        if seg["name"] != "codeSEG":
            continue
        boot_merged = False
        if load_seg is not None:
            boot_size = load_seg["rom_end"] - seg["rom_start"]
            seg = {
                **seg,
                "rom_end": load_seg["rom_end"],
                "size": boot_size,
                "name": "codeSEG",
                "is_overlay": False,
            }
            boot_merged = True
        section = build_section(
            seg,
            rom,
            args.split_functions,
            boot_merged=boot_merged,
            merge_inward_branches=merge_inward,
        )
        if section is not None and args.split_functions:
            existing = {fn["vram"] for fn in section["functions"]}
            for gap in CODESEG_GAP_SYMBOLS:
                if gap["vram"] not in existing:
                    section["functions"].append(
                        {"name": gap["name"], "vram": gap["vram"], "size": gap["size"]}
                    )
                    existing.add(gap["vram"])
            section["functions"].sort(key=lambda fn: fn["vram"])
        append_section(section)
        break

    # PI DMA overlays: boot_dma1 slice, layout bank/course blobs, prog overlays.
    use_boot_dma = args.boot_overlays_only or args.boot_dma_slices
    if use_boot_dma:
        for slice_def in BOOT_DMA_SLICES:
            append_section(
                build_dma_slice_section(
                    slice_def, rom, args.split_functions, merge_inward_branches=merge_inward
                )
            )

    if args.layout_overlays:
        overlay_candidates = [
            s for s in all_segments if is_dma_relocatable_segment(s) and s["name"] != "codeSEG"
        ]
        for seg in overlay_candidates[: args.max_overlay_sections]:
            append_section(
                build_section(seg, rom, args.split_functions, merge_inward_branches=merge_inward)
            )

    if args.prog_overlays:
        for slice_def in PROG_OVERLAY_SLICES:
            append_section(
                build_dma_slice_section(
                    slice_def, rom, args.split_functions, merge_inward_branches=merge_inward
                )
            )

    write_syms_toml(args.out, sections_out)

    fn_count = sum(len(s["functions"]) for s in sections_out)
    print(f"Wrote {len(sections_out)} sections / {fn_count} functions -> {args.out}")

    if use_boot_dma and not args.layout_overlays and not args.prog_overlays:
        print(
            f"Syms include {len(overlay_names)} boot DMA slice(s); "
            "run with --overlays-list-only --layout-overlays --prog-overlays to refresh overlays.us.txt"
        )
    else:
        args.overlays_out.write_text("\n".join(overlay_names) + ("\n" if overlay_names else ""), encoding="utf-8")
        print(f"Overlay list ({len(overlay_names)} sections) -> {args.overlays_out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
