#!/usr/bin/env python3
"""Regression tests for tools/project_inventory.py."""

from __future__ import annotations

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import project_inventory as inventory  # noqa: E402


class ProjectInventoryTests(unittest.TestCase):
    def test_live_repository_inventory(self) -> None:
        current = inventory.collect()
        self.assertGreater(len(current.syscalls), 0)
        self.assertEqual(
            [number for number, _name in current.syscalls],
            list(range(1, len(current.syscalls) + 1)),
        )
        self.assertIn("stress", current.user_programs)
        self.assertIn("tests/test_process", current.native_suites)
        self.assertIn("tests/test_fd_dup", current.standalone_native_suites)
        self.assertIn("test-stress", current.qemu_targets)
        self.assertGreater(current.stress_mutations, 0)

    def test_makefile_continuations_are_joined(self) -> None:
        slash = "\\"
        text = (
            f"PROGRAMS = one.elf two.elf {slash}\n"
            f"\tthree.elf {slash}\n"
            "\tfour.elf\n"
        )
        self.assertEqual(
            inventory.parse_make_variable(text, "PROGRAMS"),
            ("one.elf", "two.elf", "three.elf", "four.elf"),
        )

    def test_executed_shell_parser_ignores_bash_n(self) -> None:
        text = """
        bash -n tests/run_fd_dup_test.sh
        bash tests/test_user_incremental_build.sh
        bash tests/run_fd_dup_test.sh
        """
        self.assertEqual(
            inventory.parse_executed_shell_scripts(text),
            (
                "tests/test_user_incremental_build.sh",
                "tests/run_fd_dup_test.sh",
            ),
        )

    def test_native_test_target_parser_requires_declared_source_contract(self) -> None:
        runner = r'''
        NATIVE_TEST_SOURCE=tests/test_fd_dup.c
        "$cc" "$repo_dir/$NATIVE_TEST_SOURCE" -o "$tmp_dir/test_fd_dup"
        # Repeated mentions do not create duplicate gates.
        echo tests/test_fd_dup.c
        '''
        self.assertEqual(
            inventory.parse_native_test_targets(runner),
            ("tests/test_fd_dup",),
        )

    def test_native_test_target_parser_ignores_bare_source_mentions(self) -> None:
        runner = r'''
        # This runner used to compile tests/test_fd_dup.c.
        echo tests/test_fd_dup.c
        '''
        self.assertEqual(inventory.parse_native_test_targets(runner), ())

    def test_native_test_target_parser_rejects_unused_contract(self) -> None:
        runner = r'''
        NATIVE_TEST_SOURCE=tests/test_fd_dup.c
        "$cc" tests/test_other.c -o "$tmp_dir/test_other"
        '''
        with self.assertRaisesRegex(inventory.InventoryError, "declares but does not use"):
            inventory.parse_native_test_targets(runner)

    def test_native_test_target_parser_rejects_duplicate_contract(self) -> None:
        runner = r'''
        NATIVE_TEST_SOURCE=tests/test_one.c
        NATIVE_TEST_SOURCE=tests/test_two.c
        "$cc" "$repo_dir/$NATIVE_TEST_SOURCE" -o "$tmp_dir/test"
        '''
        with self.assertRaisesRegex(inventory.InventoryError, "exactly once"):
            inventory.parse_native_test_targets(runner)

    def test_syscall_parser_rejects_a_gap(self) -> None:
        text = """
        enum syscall_number {
            SYS_ONE = 1,
            SYS_THREE = 3,
        };
        """
        with self.assertRaisesRegex(inventory.InventoryError, "contiguous"):
            inventory.parse_syscalls(text)

    def test_syscall_parser_rejects_duplicate_number(self) -> None:
        text = """
        enum syscall_number {
            SYS_ONE = 1,
            SYS_OTHER = 1,
        };
        """
        with self.assertRaisesRegex(inventory.InventoryError, "duplicate syscall number"):
            inventory.parse_syscalls(text)

    def test_ramfs_registration_rejects_name_symbol_mismatch(self) -> None:
        kernel = '''
        ramfs_create_static_file("hello", cat_elf_data, cat_elf_size);
        '''
        with self.assertRaisesRegex(inventory.InventoryError, "name/symbol mismatch"):
            inventory.registered_programs(kernel)

    def test_ramfs_registration_accepts_matching_symbol(self) -> None:
        kernel = '''
        ramfs_create_static_file("hello", hello_elf_data, hello_elf_size);
        ramfs_create_static_file("cat", cat_elf_data, cat_elf_size);
        '''
        self.assertEqual(inventory.registered_programs(kernel), {"hello", "cat"})

    def test_require_same_reports_missing_and_extra(self) -> None:
        with self.assertRaises(inventory.InventoryError) as caught:
            inventory.require_same("programs", {"one", "two"}, {"two", "three"})
        message = str(caught.exception)
        self.assertIn("missing one", message)
        self.assertIn("extra three", message)

    def test_orphan_native_test_source_is_reported(self) -> None:
        with self.assertRaises(inventory.InventoryError) as caught:
            inventory.require_same(
                "native C test sources disagree with registered unit/standalone gates",
                {"tests/test_one", "tests/test_orphan"},
                {"tests/test_one"},
            )
        self.assertIn("missing tests/test_orphan", str(caught.exception))

    def test_require_substrings_rejects_stale_documentation(self) -> None:
        with self.assertRaisesRegex(inventory.InventoryError, "stale/missing"):
            inventory.require_substrings(
                "README.md",
                "51 system calls",
                ("51 system calls", "53 user programs / demos"),
            )


if __name__ == "__main__":
    unittest.main()
