#!/usr/bin/env python3
"""Regression tests for tools/check_gnu_stack_notes.py."""

from __future__ import annotations

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import check_gnu_stack_notes as stack_notes  # noqa: E402


class GnuStackNoteTests(unittest.TestCase):
    def test_live_repository_assembly_is_covered(self) -> None:
        report = stack_notes.collect_and_validate()
        self.assertIn("boot.s", report.assembly_sources)
        self.assertIn("user/crt0.s", report.assembly_sources)
        self.assertGreaterEqual(len(report.assembly_sources), 7)

    def test_valid_non_executable_note_is_accepted(self) -> None:
        stack_notes.validate_source_text(
            "valid.s",
            '.text\nret\n.section .note.GNU-stack, "", @progbits\n',
        )

    def test_missing_note_is_rejected(self) -> None:
        with self.assertRaisesRegex(stack_notes.StackNoteError, "missing"):
            stack_notes.validate_source_text("missing.s", ".text\nret\n")

    def test_executable_stack_flag_is_rejected(self) -> None:
        with self.assertRaisesRegex(stack_notes.StackNoteError, "empty flags"):
            stack_notes.validate_source_text(
                "exec.s",
                '.section .note.GNU-stack,"x",@progbits\n',
            )

    def test_duplicate_notes_are_rejected(self) -> None:
        text = (
            '.section .note.GNU-stack,"",@progbits\n'
            '.section .note.GNU-stack,"",@progbits\n'
        )
        with self.assertRaisesRegex(stack_notes.StackNoteError, "duplicate"):
            stack_notes.validate_source_text("duplicate.s", text)


if __name__ == "__main__":
    unittest.main()
