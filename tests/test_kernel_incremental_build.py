#!/usr/bin/env python3
"""Build the real kernel and compare header-only updates with a clean build."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
INPUT_TIME = 946684800       # 2000-01-01
OUTPUT_TIME = 978307200      # 2001-01-01
EDIT_TIME = 1009843200       # 2002-01-01


class KernelIncrementalBuildTests(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory(prefix="minios-kernel-deps-")
        self.addCleanup(temporary.cleanup)
        self.build = Path(temporary.name) / "repo"
        shutil.copytree(
            ROOT,
            self.build,
            ignore=shutil.ignore_patterns(
                ".git", "*.o", "*.d", "*.elf", "*_embed.c", "kernel.bin",
                "ata-test.img", "fat16.img", "*.iso", "isodir", "*.log",
                "__pycache__",
            ),
        )
        self.env = os.environ.copy()
        # These are independent makes, not participants in the outer CI
        # jobserver. Preserve CC explicitly, including cross-compiler options.
        for name in ("MAKEFLAGS", "MFLAGS", "MAKELEVEL", "GNUMAKEFLAGS"):
            self.env.pop(name, None)
        self.make_command = [
            "make", "--no-print-directory", "CC=" + os.environ.get("CC", "gcc")
        ]

    def make(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            self.make_command + list(args), cwd=self.build, env=self.env,
            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=120,
        )

    def build_ok(self, label: str, *args: str) -> None:
        result = self.make(*args)
        self.assertEqual(result.returncode, 0,
                         f"{label}: make failed\n{result.stdout}")

    def query(self, status: int, label: str, *targets: str) -> None:
        result = self.make("-q", *targets)
        self.assertEqual(result.returncode, status,
                         f"{label}: wrong freshness result\n{result.stdout}")

    def digest(self, name: str) -> str:
        return hashlib.sha256((self.build / name).read_bytes()).hexdigest()

    def outputs(self) -> dict[str, int]:
        return {
            str(path.relative_to(self.build)): path.stat().st_mtime_ns
            for path in self.build.rglob("*")
            if path.is_file() and (
                path.suffix in (".o", ".d", ".elf") or
                path.name.endswith("_embed.c") or
                path.name in ("kernel.bin", "fat16.img")
            )
        }

    def normalize_times(self) -> None:
        # No sleeps or clock-resolution assumptions: inputs < outputs < edits.
        outputs = self.outputs()
        for path in self.build.rglob("*"):
            if path.is_file():
                is_output = str(path.relative_to(self.build)) in outputs
                stamp = OUTPUT_TIME if is_output else INPUT_TIME
                os.utime(path, (stamp, stamp))

    def edit(self, name: str, old: str, new: str) -> None:
        path = self.build / name
        text = path.read_text()
        self.assertEqual(text.count(old), 1, f"edit fixture drift: {name}")
        path.write_text(text.replace(old, new))
        os.utime(path, (EDIT_TIME, EDIT_TIME))

    def assert_dependencies(self) -> None:
        for obj in self.build.glob("*.o"):
            if obj.with_suffix(".c").is_file():
                self.assertTrue(obj.with_suffix(".d").is_file(),
                                f"missing dependency file for {obj.name}")

    def assert_unchanged(self, before: dict[str, int], *names: str) -> None:
        after = self.outputs()
        for name in names:
            self.assertEqual(after[name], before[name],
                             f"unrelated artifact rebuilt: {name}")

    def test_kernel_header_updates(self) -> None:
        self.build_ok("initial parallel build", "-j4", "all")
        self.assert_dependencies()
        self.normalize_times()
        self.query(0, "initial build", "all")
        before = self.outputs()
        self.build_ok("unchanged build", "all")
        self.assertEqual(self.outputs(), before, "no-op build changed artifacts")

        original_kernel = self.digest("kernel.bin")
        original_timer = self.digest("timer.o")
        self.edit("task.h", "#define TASK_KILL_STATUS (-130)",
                  "#define TASK_KILL_STATUS (-131)")
        self.query(1, "direct header edit", "kernel.bin")
        self.build_ok("direct header rebuild", "-j4", "all")
        self.assertNotEqual(self.digest("timer.o"), original_timer,
                            "header edit retained old kill status in timer.o")
        self.assertNotEqual(self.digest("kernel.bin"), original_kernel,
                            "header edit did not reach the linked kernel")
        self.assert_unchanged(before, "rtc.o", "pmm.o", "hello_embed.o")

        # procfs.c includes process.h, which includes paging.h. A direct-only
        # header list would miss this dependency even though the types are shared.
        self.normalize_times()
        before = self.outputs()
        self.edit("paging.h", "#define PAGING_H", "#define PAGING_H\n/* dependency edit */")
        self.query(1, "transitive header edit", "procfs.o")
        self.build_ok("transitive header rebuild", "-j4", "all")
        after = self.outputs()
        for name in ("procfs.o", "kernel.bin"):
            self.assertNotEqual(after[name], before[name],
                                f"transitive header edit did not rebuild {name}")
        self.assert_unchanged(before, "rtc.o", "pmm.o", "hello_embed.o")

        # A changed Makefile must also migrate old objects that have no .d
        # files yet; otherwise pulling this fix can keep the broken old graph.
        self.normalize_times()
        before = self.outputs()
        for path in self.build.glob("*.d"):
            path.unlink()
        self.edit("Makefile", "CC   = gcc", "CC   = gcc\n# dependency bootstrap")
        self.query(1, "build rule edit", "kernel.bin")
        self.build_ok("build rule rebuild", "-j4", "all")
        self.assert_dependencies()
        after = self.outputs()
        for name in ("boot.o", "rtc.o", "hello_embed.o"):
            self.assertNotEqual(after[name], before[name],
                                f"build rule edit did not rebuild {name}")

        # Dependency generation must survive callers overriding CFLAGS.
        (self.build / "rtc.o").unlink()
        (self.build / "rtc.d").unlink()
        self.build_ok("CFLAGS override", "rtc.o",
                      "CFLAGS=-m32 -std=gnu99 -ffreestanding -fno-pie "
                      "-fno-stack-protector -O2 -Wall -Wextra")
        self.assertTrue((self.build / "rtc.d").is_file(),
                        "CFLAGS override disabled dependency generation")

        # Remove a now-unused header while its old name is still in the .d
        # file. -MP must allow recompilation to replace that stale dependency.
        obsolete = self.build / "dependency_obsolete.h"
        obsolete.write_text("/* temporary dependency */\n")
        self.normalize_times()
        include = '#include "dependency_obsolete.h"\n'
        self.edit("rtc.h", "#define RTC_H\n", "#define RTC_H\n" + include)
        self.build_ok("new header rebuild", "-j4", "all")
        self.assertIn(obsolete.name, (self.build / "rtc.d").read_text(),
                      "new transitive header was not recorded")
        self.normalize_times()
        self.edit("rtc.h", include, "")
        obsolete.unlink()
        self.build_ok("removed header rebuild", "-j4", "all")
        self.assertNotIn(obsolete.name, (self.build / "rtc.d").read_text(),
                         "removed header remains in regenerated dependencies")
        self.query(0, "completed incremental build", "all")

        incremental_kernel = self.digest("kernel.bin")
        # A same-named file must not suppress the phony clean recipe.
        (self.build / "clean").touch()
        self.build_ok("clean", "clean")
        self.assertFalse(list(self.build.rglob("*.d")), "clean left dependency files")
        self.assertFalse(list(self.build.rglob("*.o")), "clean left object files")
        self.assertFalse((self.build / "kernel.bin").exists(), "clean left the kernel")
        self.build_ok("final clean build", "-j4", "all")
        self.assertEqual(self.digest("kernel.bin"), incremental_kernel,
                         "incremental kernel differs from the clean build")
        self.query(0, "final clean build", "all")
        print("kernel incremental dependency regression passed", flush=True)


if __name__ == "__main__":
    unittest.main()
