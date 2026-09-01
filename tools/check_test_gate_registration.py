#!/usr/bin/env python3
"""Require every Python/shell regression source to be executed by static analysis."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re
import sys
from typing import Iterable

ROOT = Path(__file__).resolve().parents[1]


class RegistrationError(RuntimeError):
    """Raised when test sources and static-analysis execution drift apart."""


@dataclass(frozen=True)
class GateReport:
    python_tests: tuple[str, ...]
    shell_tests: tuple[str, ...]


def parse_direct_python_tests(text: str) -> tuple[str, ...]:
    """Return test modules directly executed as ``python3 tests/test_*.py``."""
    return tuple(
        re.findall(
            r"^\s*python3\s+(tests/test_[A-Za-z0-9_]+\.py)\s*$",
            text,
            re.M,
        )
    )


def parse_direct_shell_tests(text: str) -> tuple[str, ...]:
    """Return shell regressions directly executed as ``bash tests/test_*.sh``."""
    return tuple(
        re.findall(
            r"^\s*bash\s+(tests/test_[A-Za-z0-9_.-]+\.sh)\s*$",
            text,
            re.M,
        )
    )


def require_exact(label: str, expected: Iterable[str], actual: Iterable[str]) -> None:
    expected_tuple = tuple(expected)
    actual_tuple = tuple(actual)

    duplicates = sorted({item for item in actual_tuple if actual_tuple.count(item) > 1})
    if duplicates:
        raise RegistrationError(
            f"{label} contains duplicate execution: " + ", ".join(duplicates)
        )

    expected_set = set(expected_tuple)
    actual_set = set(actual_tuple)
    if expected_set == actual_set:
        return

    pieces: list[str] = []
    missing = sorted(expected_set - actual_set)
    extra = sorted(actual_set - expected_set)
    if missing:
        pieces.append("missing execution for " + ", ".join(missing))
    if extra:
        pieces.append("executes unknown test " + ", ".join(extra))
    raise RegistrationError(f"{label}: " + "; ".join(pieces))


def collect_and_validate(root: Path = ROOT) -> GateReport:
    static_gate = (root / "tests/run_static_analysis.sh").read_text(encoding="utf-8")

    python_sources = tuple(
        f"tests/{path.name}" for path in sorted((root / "tests").glob("test_*.py"))
    )
    shell_sources = tuple(
        f"tests/{path.name}" for path in sorted((root / "tests").glob("test_*.sh"))
    )

    python_exec = parse_direct_python_tests(static_gate)
    shell_exec = parse_direct_shell_tests(static_gate)

    require_exact("Python regression registration", python_sources, python_exec)
    require_exact("shell regression registration", shell_sources, shell_exec)

    return GateReport(python_tests=python_sources, shell_tests=shell_sources)


def main() -> int:
    try:
        report = collect_and_validate()
    except (OSError, RegistrationError) as exc:
        print(f"test gate registration failed: {exc}", file=sys.stderr)
        return 1

    print(
        "test gate registration passed: "
        f"{len(report.python_tests)} Python + {len(report.shell_tests)} shell regressions"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
