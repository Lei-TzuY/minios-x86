#!/usr/bin/env python3
"""Regression tests for tools/check_syscall_abi.py."""

from __future__ import annotations

from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import check_syscall_abi as abi  # noqa: E402


class SyscallAbiTests(unittest.TestCase):
    def test_live_repository_syscall_abi(self) -> None:
        report = abi.collect_and_validate()
        self.assertEqual(
            report.wrappers,
            report.kernel_syscalls - len(abi.NON_WRAPPER_SYSCALLS),
        )
        self.assertEqual(report.dispatch_cases, report.kernel_syscalls)
        self.assertGreater(report.named_direct_sites, 0)

    def test_wrapper_parser_maps_function_name_to_immediate(self) -> None:
        header = r'''
        static inline int sys_write(const char *buf, int len) {
            int ret;
            __asm__ volatile("int $0x80"
                             : "=a"(ret)
                             : "a"(1), "b"(buf), "c"(len)
                             : "memory");
            return ret;
        }
        '''
        self.assertEqual(abi.parse_wrapper_immediates(header), {"SYS_WRITE": 1})

    def test_validate_rejects_wrong_wrapper_number(self) -> None:
        kernel = ((1, "SYS_WRITE"), (4, "SYS_EXEC"), (24, "SYS_SIGRETURN"))
        with self.assertRaisesRegex(abi.AbiError, "wrapper=2, kernel=1"):
            abi.validate(kernel, {"SYS_WRITE": 2}, ())

    def test_validate_rejects_missing_wrapper(self) -> None:
        kernel = (
            (1, "SYS_WRITE"),
            (2, "SYS_READ"),
            (4, "SYS_EXEC"),
            (24, "SYS_SIGRETURN"),
        )
        with self.assertRaisesRegex(abi.AbiError, "missing user wrappers: SYS_READ"):
            abi.validate(kernel, {"SYS_WRITE": 1}, ())

    def test_validate_rejects_wrong_named_direct_site(self) -> None:
        kernel = ((3, "SYS_EXIT"), (4, "SYS_EXEC"), (24, "SYS_SIGRETURN"))
        with self.assertRaisesRegex(abi.AbiError, "direct immediate=4, kernel=3"):
            abi.validate(
                kernel,
                {"SYS_EXIT": 3},
                (("SYS_EXIT", 4, "user/crt0.s", 12),),
            )

    def test_validate_rejects_missing_kernel_dispatch_case(self) -> None:
        kernel = ((1, "SYS_WRITE"), (4, "SYS_EXEC"), (24, "SYS_SIGRETURN"))
        with self.assertRaisesRegex(abi.AbiError, "missing dispatch cases: SYS_EXEC"):
            abi.validate(
                kernel,
                {"SYS_WRITE": 1},
                (),
                ("SYS_WRITE", "SYS_SIGRETURN"),
            )

    def test_dispatch_parser_scopes_to_syscall_handler(self) -> None:
        source = r'''
        static int unrelated(int whence) {
            switch (whence) {
                case SYS_SEEK_SET: return 0;
                default: return -1;
            }
        }

        static void syscall_handler(registers_t *regs) {
            switch (regs->eax) {
                case SYS_WRITE:
                    break;
                case SYS_READ:
                    break;
                default:
                    break;
            }
        }

        void syscall_install(void) {
        }
        '''
        self.assertEqual(
            abi.parse_dispatch_cases(source),
            ("SYS_WRITE", "SYS_READ"),
        )

    def test_named_direct_site_parser_uses_sys_comment(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "crt0.s"
            path.write_text(
                "mov $3, %eax /* SYS_EXIT */\n"
                "mov $24, %eax /* SYS_SIGRETURN */\n",
                encoding="utf-8",
            )
            self.assertEqual(
                [(name, number) for name, number, _path, _line in abi.parse_named_direct_sites((path,))],
                [("SYS_EXIT", 3), ("SYS_SIGRETURN", 24)],
            )

    def test_named_direct_site_parser_ignores_interrupt_vector_comment(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "sigflags.c"
            path.write_text(
                "/* SYS_SIGRETURN is reachable via int $0x80 directly. */\n"
                '"movl $24, %%eax\\n\\t" /* SYS_SIGRETURN */\n',
                encoding="utf-8",
            )
            self.assertEqual(
                [(name, number) for name, number, _path, _line in abi.parse_named_direct_sites((path,))],
                [("SYS_SIGRETURN", 24)],
            )

    def test_named_direct_site_parser_accepts_double_percent_eax(self) -> None:
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "inline.c"
            path.write_text(
                '"movl $24, %%eax\\n\\t" /* SYS_SIGRETURN */\n',
                encoding="utf-8",
            )
            sites = abi.parse_named_direct_sites((path,))
            self.assertEqual(
                [(name, number) for name, number, _path, _line in sites],
                [("SYS_SIGRETURN", 24)],
            )


if __name__ == "__main__":
    unittest.main()
