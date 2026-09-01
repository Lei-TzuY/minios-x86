#!/usr/bin/env python3
"""Regression tests for tools/check_test_gate_registration.py."""

from __future__ import annotations

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import check_test_gate_registration as gate  # noqa: E402


class TestGateRegistrationTests(unittest.TestCase):
    def test_live_repository_registration(self) -> None:
        report = gate.collect_and_validate()
        self.assertIn("tests/test_project_inventory.py", report.python_tests)
        self.assertIn("tests/test_syscall_abi.py", report.python_tests)
        self.assertIn("tests/test_test_gate_registration.py", report.python_tests)
        self.assertEqual(report.shell_tests, ("tests/test_user_incremental_build.sh",))

    def test_python_parser_ignores_py_compile(self) -> None:
        text = """
        python3 -m py_compile tests/test_one.py tests/test_two.py
        python3 tests/test_one.py
        """
        self.assertEqual(gate.parse_direct_python_tests(text), ("tests/test_one.py",))

    def test_shell_parser_ignores_bash_n(self) -> None:
        text = """
        bash -n tests/test_one.sh tests/test_two.sh
        bash tests/test_one.sh
        """
        self.assertEqual(gate.parse_direct_shell_tests(text), ("tests/test_one.sh",))

    def test_missing_execution_is_rejected(self) -> None:
        with self.assertRaisesRegex(gate.RegistrationError, "missing execution"):
            gate.require_exact(
                "Python regression registration",
                ("tests/test_one.py", "tests/test_two.py"),
                ("tests/test_one.py",),
            )

    def test_unknown_execution_is_rejected(self) -> None:
        with self.assertRaisesRegex(gate.RegistrationError, "unknown test"):
            gate.require_exact(
                "shell regression registration",
                ("tests/test_one.sh",),
                ("tests/test_one.sh", "tests/test_ghost.sh"),
            )

    def test_duplicate_execution_is_rejected(self) -> None:
        with self.assertRaisesRegex(gate.RegistrationError, "duplicate execution"):
            gate.require_exact(
                "Python regression registration",
                ("tests/test_one.py",),
                ("tests/test_one.py", "tests/test_one.py"),
            )


if __name__ == "__main__":
    unittest.main()
