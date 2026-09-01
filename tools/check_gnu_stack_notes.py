#!/usr/bin/env python3
"""Require every hand-written assembly source to declare a non-executable GNU stack."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]


class StackNoteError(RuntimeError):
    """Raised when assembly GNU-stack metadata is missing or ambiguous."""


@dataclass(frozen=True)
class StackNoteReport:
    assembly_sources: tuple[str, ...]


STACK_NOTE_RE = re.compile(
    r'^\s*\.section\s+\.note\.GNU-stack\s*,\s*""\s*,\s*@progbits\s*(?:[#;].*)?$',
    re.M,
)


def validate_source_text(label: str, text: str) -> None:
    """Require exactly one non-executable `.note.GNU-stack` declaration."""
    matches = STACK_NOTE_RE.findall(text)
    if len(matches) == 1:
        return
    if len(matches) > 1:
        raise StackNoteError(f"{label}: duplicate non-executable GNU-stack declarations")
    if ".note.GNU-stack" in text:
        raise StackNoteError(
            f"{label}: GNU-stack declaration must use empty flags and @progbits"
        )
    raise StackNoteError(f"{label}: missing non-executable GNU-stack declaration")


def collect_and_validate(root: Path = ROOT) -> StackNoteReport:
    sources = tuple(
        sorted(
            path
            for path in root.rglob("*.s")
            if ".git" not in path.parts
        )
    )
    if not sources:
        raise StackNoteError("no assembly sources found")

    labels: list[str] = []
    for path in sources:
        label = path.relative_to(root).as_posix()
        validate_source_text(label, path.read_text(encoding="utf-8"))
        labels.append(label)

    return StackNoteReport(assembly_sources=tuple(labels))


def main() -> int:
    try:
        report = collect_and_validate()
    except (OSError, StackNoteError) as exc:
        print(f"GNU-stack metadata check failed: {exc}", file=sys.stderr)
        return 1

    print(
        "GNU-stack metadata check passed: "
        f"{len(report.assembly_sources)} assembly sources"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
