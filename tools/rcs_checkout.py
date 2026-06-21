#!/usr/bin/env python3
"""Extract working files from RCS/CVS ,v archives in the WR64 leak."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


def extract_rcs_text(data: str) -> str | None:
    """Return the newest non-empty revision body from an RCS ,v file."""
    if not data.startswith("head"):
        return None

    head_match = re.search(r"^head\s+([\d.]+);", data, flags=re.MULTILINE)
    preferred_rev = head_match.group(1) if head_match else None

    rev_blocks = re.findall(
        r"^(\d+(?:\.\d+)*)\r?\nlog\r?\n@[\s\S]*?@\r?\ntext\r?\n@([\s\S]*?)@\r?\n",
        data,
        flags=re.MULTILINE,
    )
    if not rev_blocks:
        return None

    def normalize(body: str) -> str:
        return body.replace("\n@", "\n").rstrip("\n")

    def rev_key(rev: str) -> tuple[int, ...]:
        return tuple(int(part) for part in rev.split("."))

    nonempty = [(rev, normalize(body)) for rev, body in rev_blocks if body.strip()]
    if preferred_rev:
        for rev, body in nonempty:
            if rev == preferred_rev:
                return body

    if nonempty:
        rev, body = max(nonempty, key=lambda item: rev_key(item[0]))
        return body

    # Fall back to head revision even if empty (rare placeholder files).
    for rev, body in rev_blocks:
        if rev == preferred_rev:
            return normalize(body)
    return normalize(rev_blocks[0][1])


def checkout_tree(source: Path, dest: Path, force: bool) -> tuple[int, int]:
    dest.mkdir(parents=True, exist_ok=True)
    checked_out = 0
    skipped = 0

    for rcs_path in sorted(source.rglob("*,v")):
        rel = rcs_path.relative_to(source)
        out_name = rel.as_posix()[:-2]  # drop ,v
        out_path = dest / out_name
        out_path.parent.mkdir(parents=True, exist_ok=True)

        if out_path.exists() and not force:
            skipped += 1
            continue

        text = rcs_path.read_text(encoding="utf-8", errors="replace")
        body = extract_rcs_text(text)
        if body is None:
            skipped += 1
            continue

        out_path.write_text(body, encoding="utf-8", newline="\n")
        checked_out += 1

    return checked_out, skipped


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source",
        type=Path,
        default=Path(r"h:\wr64"),
        help="Path to the RCS leak tree",
    )
    parser.add_argument(
        "--dest",
        type=Path,
        default=Path(__file__).resolve().parent.parent / "leak-ref",
        help="Output directory for checked-out files",
    )
    parser.add_argument("--force", action="store_true", help="Overwrite existing files")
    args = parser.parse_args()

    if not args.source.is_dir():
        print(f"Source not found: {args.source}", file=sys.stderr)
        return 1

    checked_out, skipped = checkout_tree(args.source, args.dest, args.force)
    print(f"Checked out {checked_out} files, skipped {skipped}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
