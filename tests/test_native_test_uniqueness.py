#!/usr/bin/env python3
"""Regression tests for tools/check_native_test_uniqueness.py."""

from __future__ import annotations

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import check_native_test_uniqueness as ownership  # noqa: E402


class NativeTestUniquenessTests(unittest.TestCase):
    def test_live_repository_ownership(self) -> None:
        report = ownership.collect_and_validate()
        self.assertIn("tests/test_fdtable", report.unit_suites)
        self.assertIn(
            ("tests/test_fd_dup", "tests/run_fd_dup_test.sh"),
            report.standalone_owners,
        )

    def test_unit_and_standalone_overlap_is_rejected(self) -> None:
        with self.assertRaisesRegex(ownership.OwnershipError, "both UNIT_BINS"):
            ownership.validate_unique(
                ("tests/test_one",),
                (("tests/test_one", "tests/run_one.sh"),),
            )

    def test_two_standalone_owners_are_rejected(self) -> None:
        with self.assertRaisesRegex(ownership.OwnershipError, "owned by both"):
            ownership.validate_unique(
                (),
                (
                    ("tests/test_one", "tests/run_one.sh"),
                    ("tests/test_one", "tests/run_other.sh"),
                ),
            )

    def test_distinct_owners_are_accepted(self) -> None:
        report = ownership.validate_unique(
            ("tests/test_unit",),
            (
                ("tests/test_a", "tests/run_a.sh"),
                ("tests/test_b", "tests/run_b.sh"),
            ),
        )
        self.assertEqual(len(report.unit_suites), 1)
        self.assertEqual(len(report.standalone_owners), 2)


if __name__ == "__main__":
    unittest.main()
