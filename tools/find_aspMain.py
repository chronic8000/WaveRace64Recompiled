#!/usr/bin/env python3
"""Brute-find aspMain by running RSPRecomp on candidate ROM offsets."""

from __future__ import annotations

import subprocess
import tempfile
from pathlib import Path

from rom_util import load_rom

CODE_END = 0x1000 + 521712
TEXT_SIZE = 0x1000
TEXT_ADDR = 0x04001000


def try_offset(root: Path, rsp: Path, rom_name: str, off: int) -> bool:
    toml = root / "aspMain.wr64.toml"
    out = root / "rsp" / "aspMain.cpp"
    toml.write_text(
        f"""text_offset = 0x{off:X}
text_size = 0x{TEXT_SIZE:X}
text_address = 0x{TEXT_ADDR:X}
rom_file_path = "{rom_name}"
output_file_path = "rsp/aspMain.cpp"
output_function_name = "aspMain"
extra_indirect_branch_targets = []
""",
        encoding="utf-8",
    )
    if out.exists():
        out.unlink()
    proc = subprocess.run([str(rsp), str(toml)], cwd=root, capture_output=True, text=True)
    if proc.returncode == 0 and out.is_file() and out.stat().st_size > 1000:
        print(f"SUCCESS at ROM offset 0x{off:X}")
        return True
    return False


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    rsp = root / "N64Recomp" / "build" / "Release" / "RSPRecomp.exe"
    if not rsp.is_file():
        print(f"Missing {rsp}")
        return 1

    rom, _ = load_rom(root / "wr64.us.revA.z64")
    # Search last 256 KiB of codeSEG, 0x100 aligned
    start = CODE_END - 0x40000
    end = CODE_END - TEXT_SIZE
    for off in range(end, start, -0x10):
        if try_offset(root, rsp, "wr64.us.revA.z64", off):
            return 0
    print("No aspMain offset found")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
