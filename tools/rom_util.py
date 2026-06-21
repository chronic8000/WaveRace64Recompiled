#!/usr/bin/env python3
"""Normalize N64 ROM images to big-endian .z64 layout."""

from __future__ import annotations

import struct
from pathlib import Path


def bswap16(data: bytes) -> bytes:
    out = bytearray(len(data))
    for i in range(0, len(data) - 1, 2):
        out[i] = data[i + 1]
        out[i + 1] = data[i]
    if len(data) % 2:
        out[-1] = data[-1]
    return bytes(out)


def wordswap(data: bytes) -> bytes:
    out = bytearray(len(data))
    for i in range(0, len(data) - 3, 4):
        out[i : i + 4] = data[i + 2 : i + 4] + data[i : i + 2]
    return bytes(out)


def detect_format(data: bytes) -> str:
    if len(data) < 0x40:
        return "unknown"
    pc_be = struct.unpack(">I", data[0x08:0x0C])[0]
    title_be = data[0x20:0x34]
    pc_le = struct.unpack("<I", data[0x08:0x0C])[0]
    bswap = bswap16(data)
    pc_bswap = struct.unpack(">I", bswap[0x08:0x0C])[0]
    title_bswap = bswap[0x20:0x34]

    if title_bswap.startswith(b"WAVE RACE") or title_bswap.startswith(b"WAVERACE"):
        return "n64"
    if title_be.startswith(b"WAVE RACE") or title_be.startswith(b"WAVERACE"):
        return "z64"
    if 0x80000000 <= pc_bswap <= 0x80800000:
        return "n64"
    if 0x80000000 <= pc_be <= 0x80800000:
        return "z64"
    if 0x80000000 <= pc_le <= 0x80800000:
        return "v64"
    return "unknown"


def normalize_rom(data: bytes) -> tuple[bytes, str]:
    fmt = detect_format(data)
    if fmt == "z64":
        return data, fmt
    if fmt == "n64":
        return bswap16(data), fmt
    if fmt == "v64":
        return wordswap(data), fmt
    # Best effort: prefer .n64 halfword swap when PC looks wrong.
    bswap = bswap16(data)
    pc = struct.unpack(">I", bswap[0x08:0x0C])[0]
    if 0x80000000 <= pc <= 0x80800000:
        return bswap, "n64"
    return data, "unknown"


def read_header(data: bytes) -> dict:
    return {
        "pc": struct.unpack(">I", data[0x08:0x0C])[0],
        "crc": data[0x10:0x14].hex(),
        "title": data[0x20:0x34].decode("ascii", errors="replace").strip(),
        "size": len(data),
    }


def load_rom(path: Path) -> tuple[bytes, dict]:
    raw = path.read_bytes()
    normalized, fmt = normalize_rom(raw)
    header = read_header(normalized)
    header["source_format"] = fmt
    header["source_path"] = str(path)
    return normalized, header
