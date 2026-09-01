#!/usr/bin/env python3
"""Reject ambiguous ownership of native C regression sources."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import sys
from typing import Iterable

import project_inventory

ROOT = Path(__file__).resolve().parents[1]


class OwnershipError(RuntimeError):
    """Raised when one native test is registered through multiple gate paths."""


@dataclass(frozen=True)
class OwnershipReport:
    unit_suites: tuple[str, ...]
    standalone_owners: tuple[tuple[str, str], ...]


def validate_unique(
    unit_suites: Iterable[str],
    standalone_owners: Iterable[tuple[str, str]],
) -> OwnershipReport:
    units = tuple(unit_suites)
    pairs = tuple(standalone_owners)

    owners_by_target: dict[str, str] = {}
    for target, runner in pairs:
        prior = owners_by_target.get(target)
        if prior is not None:
            raise OwnershipError(
                f"standalone native test {target} is owned by both {prior} and {runner}"
            )
        owners_by_target[target] = runner

    overlap = sorted(set(units) & set(owners_by_target))
    if overlap:
        raise OwnershipError(
            "native tests may not be both UNIT_BINS and standalone gates: "
            + ", ".join(overlap)
        )

    return OwnershipReport(
        unit_suites=units,
        standalone_owners=tuple(sorted(pairs)),
    )


def collect_and_validate(root: Path = ROOT) -> OwnershipReport:
    root_make = (root / "Makefile").read_text(encoding="utf-8")
    static_gate = (root / "tests/run_static_analysis.sh").read_text(encoding="utf-8")
    units = project_inventory.parse_make_variable(root_make, "UNIT_BINS")

    owners: list[tuple[str, str]] = []
    for runner in project_inventory.parse_executed_shell_scripts(static_gate):
        runner_path = root / runner
        if not runner_path.is_file():
            raise OwnershipError(f"static-analysis executes missing script {runner}")
        for target in project_inventory.parse_native_test_targets(
            runner_path.read_text(encoding="utf-8")
        ):
            owners.append((target, runner))

    return validate_unique(units, owners)


def main() -> int:
    try:
        report = collect_and_validate()
    except (OSError, OwnershipError, project_inventory.InventoryError) as exc:
        print(f"native test ownership failed: {exc}", file=sys.stderr)
        return 1

    print(
        "native test ownership passed: "
        f"{len(report.unit_suites)} UNIT_BINS + "
        f"{len(report.standalone_owners)} standalone owners"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
