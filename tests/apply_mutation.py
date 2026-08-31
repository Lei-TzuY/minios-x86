#!/usr/bin/env python3
"""Apply one exact text mutation while preserving a source file's line endings."""

from pathlib import Path
import sys


def main() -> int:
    if len(sys.argv) != 4:
        print(f"usage: {sys.argv[0]} FILE OLD NEW", file=sys.stderr)
        return 2

    path = Path(sys.argv[1])
    raw = path.read_bytes()
    newline = b"\r\n" if b"\r\n" in raw else b"\n"
    text = raw.replace(b"\r\n", b"\n").decode("utf-8")
    old, new = sys.argv[2], sys.argv[3]
    if text.count(old) != 1:
        print(
            f"mutation pattern occurs {text.count(old)} times in {path}, expected 1",
            file=sys.stderr,
        )
        return 1
    mutated = text.replace(old, new, 1).encode("utf-8")
    if newline == b"\r\n":
        mutated = mutated.replace(b"\n", b"\r\n")
    path.write_bytes(mutated)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
