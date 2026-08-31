#!/usr/bin/env python3
"""Boot miniOS, run the ring-3 stress suite twice, and detect resource leaks."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import time


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", default=os.environ.get("QEMU", "qemu-system-i386"))
    parser.add_argument("--kernel", default="kernel.bin")
    parser.add_argument("--disk", default="ata-test.img")
    parser.add_argument("--log", default="tests/qemu-stress.log")
    parser.add_argument("--timeout", type=float, default=180.0)
    return parser.parse_args()


class HarnessError(RuntimeError):
    pass


class QemuHarness:
    def __init__(self, args: argparse.Namespace) -> None:
        self.args = args
        self.log_path = Path(args.log).resolve()
        self.log_path.parent.mkdir(parents=True, exist_ok=True)
        self.log_path.write_bytes(b"")
        self.tmp = tempfile.TemporaryDirectory(prefix="minios-qemu-")
        self.monitor_path = Path(self.tmp.name) / "monitor.sock"
        self.process: subprocess.Popen[str] | None = None
        self.monitor: socket.socket | None = None
        self.deadline = time.monotonic() + args.timeout

    def start(self) -> None:
        kernel = Path(self.args.kernel).resolve()
        disk = Path(self.args.disk).resolve()
        command = [
            self.args.qemu,
            "-rtc", "base=2020-01-01T00:00:00",
            "-drive",
            f"file={disk},format=raw,if=ide,index=0,media=disk,snapshot=on",
            "-display", "none",
            "-serial", "none",
            "-debugcon", f"file:{self.log_path}",
            "-monitor", f"unix:{self.monitor_path},server=on,wait=off",
            "-no-reboot",
            "-no-shutdown",
            "-kernel", str(kernel),
        ]
        self.process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        self._wait_until(lambda _: self.monitor_path.exists(), "QEMU monitor socket")

        monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        while True:
            try:
                monitor.connect(str(self.monitor_path))
                break
            except (ConnectionRefusedError, FileNotFoundError):
                self._check_process()
                self._sleep()
        monitor.settimeout(0.2)
        try:
            monitor.recv(4096)
        except socket.timeout:
            pass
        self.monitor = monitor
        self._wait_until(lambda text: "miniOS shell" in text, "kernel shell")

    def close(self) -> None:
        if self.monitor is not None:
            try:
                self.monitor.sendall(b"quit\n")
            except OSError:
                pass
            self.monitor.close()
            self.monitor = None
        if self.process is not None:
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.terminate()
                try:
                    self.process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=3)
            self.process = None
        self.tmp.cleanup()

    def text(self) -> str:
        try:
            return self.log_path.read_text(encoding="utf-8", errors="replace")
        except FileNotFoundError:
            return ""

    def qemu_output(self) -> str:
        if self.process is None or self.process.stdout is None:
            return ""
        if self.process.poll() is None:
            return ""
        return self.process.stdout.read()

    def send_shell_command(self, command: str) -> None:
        if self.monitor is None:
            raise HarnessError("QEMU monitor is not connected")
        for char in command:
            if "a" <= char <= "z" or "0" <= char <= "9":
                key = char
            elif char == " ":
                key = "spc"
            else:
                raise HarnessError(f"unsupported shell-command character: {char!r}")
            self.monitor.sendall(f"sendkey {key}\n".encode("ascii"))
            time.sleep(0.04)
        self.monitor.sendall(b"sendkey ret\n")

    def wait_for_count(self, marker: str, count: int, label: str) -> str:
        return self._wait_until(lambda text: text.count(marker) >= count, label)

    def wait_for_stress_result(self, run: int) -> str:
        def complete(text: str) -> bool:
            if text.count("[stress FAILED]") >= run or "System Halted." in text:
                raise HarnessError(f"stress run {run} reported a failure")
            return text.count("[stress PASS]") >= run

        return self._wait_until(complete, f"stress run {run}")

    def _wait_until(self, predicate, label: str) -> str:
        while time.monotonic() < self.deadline:
            self._check_process()
            text = self.text()
            if predicate(text):
                return text
            self._sleep()
        raise HarnessError(f"timed out waiting for {label}")

    def _check_process(self) -> None:
        if self.process is not None and self.process.poll() is not None:
            output = self.qemu_output().strip()
            detail = f": {output}" if output else ""
            raise HarnessError(f"QEMU exited unexpectedly{detail}")

    @staticmethod
    def _sleep() -> None:
        time.sleep(0.05)


SNAPSHOT_PATTERNS = {
    "pmm": re.compile(r"PMM blocks: total=(\d+) used=(\d+) free=(\d+)"),
    "heap": re.compile(r"Kernel heap: pages=(\d+) free-bytes=(\d+)"),
    "user": re.compile(r"User pages: accessible=(\d+) spaces=(\d+)"),
    "process": re.compile(r"Processes: running=(\d+) zombies=(\d+) peak=(\d+)"),
    "task": re.compile(r"Tasks: blocked=(\d+)"),
    "timer": re.compile(r"Timers: sleeping=(\d+)"),
    "ramfs": re.compile(r"RAMFS nodes=(\d+)"),
}


def snapshots(log: str) -> list[dict[str, tuple[int, ...]]]:
    matches: dict[str, list[tuple[int, tuple[int, ...]]]] = {}
    for name, pattern in SNAPSHOT_PATTERNS.items():
        matches[name] = [
            (match.start(), tuple(int(group) for group in match.groups()))
            for match in pattern.finditer(log)
        ]
        if len(matches[name]) != 2:
            raise HarnessError(
                f"expected two {name} snapshot lines, found {len(matches[name])}"
            )

    result: list[dict[str, tuple[int, ...]]] = []
    for index in range(2):
        result.append({name: values[index][1] for name, values in matches.items()})
    return result


def validate(log: str) -> None:
    required = [
        "[stress invalid pointers ok]",
        "[stress fault isolation ok]",
        "[stress heap exhaustion ok]",
        "[stress heap and paging ok]",
        "[stress fd and pipe exhaustion ok]",
        "[stress filesystems ok]",
        "[stress interrupts and preemption ok]",
        "[stress scheduling and context switches ok]",
        "[stress process exhaustion ok]",
        "[stress repeated lifecycle ok]",
        "[stress PASS]",
    ]
    for marker in required:
        if log.count(marker) != 2:
            raise HarnessError(f"expected marker twice: {marker}")

    forbidden = [
        "[stress FAILED]",
        " FAIL]",
        "System Halted.",
        "PAGE FAULT!",
        "exec: not found",
    ]
    for marker in forbidden:
        if marker in log:
            raise HarnessError(f"forbidden marker in QEMU log: {marker}")

    heap_counts = [
        int(value)
        for value in re.findall(r"\[stress heap exhaustion allocations=(\d+)\]", log)
    ]
    if len(heap_counts) != 2 or heap_counts[0] != heap_counts[1]:
        raise HarnessError(f"heap exhaustion count mismatch: {heap_counts}")

    fault_counts = [
        int(value)
        for value in re.findall(r"\[stress fault isolation iterations=(\d+)\]", log)
    ]
    if (len(fault_counts) != 2 or fault_counts[0] != fault_counts[1] or
            fault_counts[0] < 16):
        raise HarnessError(f"fault isolation count mismatch: {fault_counts}")
    expected_faults = sum(fault_counts)
    if expected_faults % 4:
        raise HarnessError(f"fault mode distribution is not even: {fault_counts}")
    expected_per_mode = expected_faults // 4
    per_mode_markers = (
        "[fault resources armed mode=page]",
        "[fault resources armed mode=divide]",
        "[fault resources armed mode=invalid]",
        "[fault resources armed mode=privileged]",
        "USER PAGE FAULT at address 2097152",
        "USER EXCEPTION 0\n",
        "USER EXCEPTION 6\n",
        "USER EXCEPTION 13\n",
    )
    for marker in per_mode_markers:
        actual = log.count(marker)
        if actual != expected_per_mode:
            raise HarnessError(
                f"expected {expected_per_mode} occurrences of {marker}, found {actual}"
            )
    actual_terminations = log.count("Terminating user program.")
    if actual_terminations != expected_faults:
        raise HarnessError(
            f"expected {expected_faults} user terminations, found {actual_terminations}"
        )

    first, second = snapshots(log)
    if first != second:
        differences = [
            f"{name}: first={first[name]} second={second[name]}"
            for name in first
            if first[name] != second[name]
        ]
        raise HarnessError("resource snapshot drift: " + "; ".join(differences))

    if first["user"] != (0, 0):
        raise HarnessError(f"user address-space leak: {first['user']}")
    if first["process"][:2] != (0, 0) or first["process"][2] != 16:
        raise HarnessError(f"process lifecycle mismatch: {first['process']}")
    if first["task"] != (0,) or first["timer"] != (0,):
        raise HarnessError(
            f"blocked task/timer leak: task={first['task']} timer={first['timer']}"
        )
    # A fresh boot has the root, three mount points, 53 user programs and
    # readme.txt.  test-shell ends at 60 because its earlier scenarios leave
    # two intentional fixtures; this isolated boot must return to 58.
    if first["ramfs"] != (58,):
        raise HarnessError(f"RAMFS node leak: {first['ramfs']}")


def main() -> int:
    args = parse_args()
    harness = QemuHarness(args)
    try:
        harness.start()
        for run in (1, 2):
            harness.send_shell_command("stress")
            harness.wait_for_stress_result(run)
            harness.wait_for_count("[program exited]", run, f"stress teardown {run}")
            harness.send_shell_command("mem")
            harness.wait_for_count("RAMFS nodes=", run, f"resource snapshot {run}")
        log = harness.text()
        validate(log)
        print(log, end="" if log.endswith("\n") else "\n")
        print("QEMU stress regression passed twice with stable resources")
        return 0
    except (HarnessError, OSError, subprocess.SubprocessError) as exc:
        log = harness.text()
        if log:
            print(log, end="" if log.endswith("\n") else "\n")
        print(f"QEMU stress regression FAILED: {exc}")
        return 1
    finally:
        harness.close()


if __name__ == "__main__":
    raise SystemExit(main())
