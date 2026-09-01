#!/usr/bin/env python3
"""Verify that ring-3 syscall sites and kernel dispatch match the syscall ABI."""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re
import sys
from typing import Iterable

import project_inventory

ROOT = Path(__file__).resolve().parents[1]
NON_WRAPPER_SYSCALLS = {"SYS_EXEC", "SYS_SIGRETURN"}
DIRECT_EAX_IMMEDIATE = re.compile(
    r"\bmovl?\s+\$(0[xX][0-9a-fA-F]+|\d+)\s*,\s*%{1,2}eax\b"
)


class AbiError(RuntimeError):
    """Raised when a user-space or kernel syscall site disagrees with the ABI."""


@dataclass(frozen=True)
class AbiReport:
    kernel_syscalls: int
    wrappers: int
    named_direct_sites: int
    dispatch_cases: int


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def find_braced_body(text: str, brace: int, label: str) -> tuple[str, int]:
    """Return a C braced body and the position just after its closing brace.

    The scanner ignores braces inside strings, character literals, and comments.
    It is intentionally small rather than a C parser, but robust enough for the
    static inline wrappers and syscall dispatcher audited by this tool.
    """
    if brace < 0 or brace >= len(text) or text[brace] != "{":
        raise AbiError(f"could not find body for {label}")

    depth = 1
    end = brace + 1
    quote: str | None = None
    escaped = False
    line_comment = False
    block_comment = False
    while end < len(text) and depth:
        ch = text[end]
        nxt = text[end + 1] if end + 1 < len(text) else ""

        if line_comment:
            if ch == "\n":
                line_comment = False
            end += 1
            continue
        if block_comment:
            if ch == "*" and nxt == "/":
                block_comment = False
                end += 2
            else:
                end += 1
            continue
        if quote is not None:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == quote:
                quote = None
            end += 1
            continue
        if ch == "/" and nxt == "/":
            line_comment = True
            end += 2
            continue
        if ch == "/" and nxt == "*":
            block_comment = True
            end += 2
            continue
        if ch in ('"', "'"):
            quote = ch
            end += 1
            continue
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
        end += 1

    if depth:
        raise AbiError(f"unterminated body for {label}")
    return text[brace + 1 : end - 1], end


def extract_inline_wrappers(text: str) -> tuple[tuple[str, str], ...]:
    """Return ``(function_name, body)`` for static inline ``sys_*`` wrappers."""
    wrappers: list[tuple[str, str]] = []
    pos = 0
    while True:
        start = text.find("static inline", pos)
        if start < 0:
            break
        brace = text.find("{", start)
        if brace < 0:
            raise AbiError("unterminated static inline declaration")
        header = text[start:brace]
        name_match = re.search(r"\b(sys_[a-zA-Z0-9_]+)\s*\(", header)
        if not name_match:
            pos = brace + 1
            continue

        function = name_match.group(1)
        body, pos = find_braced_body(text, brace, function)
        wrappers.append((function, body))
    return tuple(wrappers)


def parse_wrapper_immediates(text: str) -> dict[str, int]:
    parsed: dict[str, int] = {}
    for function, body in extract_inline_wrappers(text):
        matches = re.findall(r'"a"\s*\(\s*(0[xX][0-9a-fA-F]+|\d+)\s*\)', body)
        if not matches:
            raise AbiError(f"{function} has no numeric eax syscall constraint")
        number = int(matches[0], 0)
        syscall_name = "SYS_" + function[len("sys_") :].upper()
        if syscall_name in parsed:
            raise AbiError(f"duplicate user syscall wrapper for {syscall_name}")
        parsed[syscall_name] = number
    return parsed


def parse_named_direct_sites(paths: Iterable[Path]) -> tuple[tuple[str, int, str, int], ...]:
    """Find named assembly sites that explicitly load a syscall ID into EAX.

    Requiring the ``mov[l] $N, %eax`` shape is deliberate. User-source comments
    also mention the interrupt vector as ``int $0x80``; treating any ``$N`` on a
    line containing ``SYS_*`` as a syscall number would turn that vector into a
    bogus ABI value of 128.
    """
    sites: list[tuple[str, int, str, int]] = []
    for path in paths:
        text = path.read_text(encoding="utf-8")
        for line_no, line in enumerate(text.splitlines(), 1):
            number_match = DIRECT_EAX_IMMEDIATE.search(line)
            name_match = re.search(r"\b(SYS_[A-Z0-9_]+)\b", line)
            if number_match and name_match:
                sites.append(
                    (name_match.group(1), int(number_match.group(1), 0), str(path), line_no)
                )
    return tuple(sites)


def parse_dispatch_cases(text: str) -> tuple[str, ...]:
    """Return the SYS_* cases from the kernel's syscall_handler function only.

    syscall.c has other switches whose labels include names such as
    SYS_SEEK_SET. Extracting the function's matched braced body prevents those
    ordinary enum cases (or later functions) from becoming fake syscall cases.
    """
    start = text.find("static void syscall_handler")
    if start < 0:
        raise AbiError("could not find syscall_handler")
    brace = text.find("{", start)
    body, _end = find_braced_body(text, brace, "syscall_handler")
    return tuple(re.findall(r"^\s*case\s+(SYS_[A-Z0-9_]+)\s*:", body, re.M))


def validate(
    kernel_entries: Iterable[tuple[int, str]],
    wrapper_numbers: dict[str, int],
    direct_sites: Iterable[tuple[str, int, str, int]],
    dispatch_names: Iterable[str] | None = None,
) -> AbiReport:
    kernel = {name: number for number, name in kernel_entries}
    if len(kernel) == 0:
        raise AbiError("kernel syscall table is empty")

    errors: list[str] = []
    for name, actual in sorted(wrapper_numbers.items()):
        expected = kernel.get(name)
        if expected is None:
            errors.append(f"userspace exposes unknown wrapper {name}={actual}")
        elif actual != expected:
            errors.append(f"{name}: userspace wrapper={actual}, kernel={expected}")

    missing = sorted(set(kernel) - set(wrapper_numbers) - NON_WRAPPER_SYSCALLS)
    if missing:
        errors.append("kernel syscalls missing user wrappers: " + ", ".join(missing))

    direct_count = 0
    for name, actual, path, line_no in direct_sites:
        direct_count += 1
        expected = kernel.get(name)
        if expected is None:
            errors.append(f"{path}:{line_no}: names unknown syscall {name}")
        elif actual != expected:
            errors.append(
                f"{path}:{line_no}: {name} direct immediate={actual}, kernel={expected}"
            )

    dispatch_count = 0
    if dispatch_names is not None:
        dispatch = tuple(dispatch_names)
        dispatch_count = len(dispatch)
        duplicates = sorted({name for name in dispatch if dispatch.count(name) > 1})
        if duplicates:
            errors.append("duplicate kernel dispatch cases: " + ", ".join(duplicates))
        missing_dispatch = sorted(set(kernel) - set(dispatch))
        if missing_dispatch:
            errors.append(
                "kernel syscalls missing dispatch cases: " + ", ".join(missing_dispatch)
            )
        unknown_dispatch = sorted(set(dispatch) - set(kernel))
        if unknown_dispatch:
            errors.append(
                "kernel dispatch references unknown syscalls: " + ", ".join(unknown_dispatch)
            )

    if errors:
        raise AbiError("syscall ABI parity failed:\n  " + "\n  ".join(errors))

    return AbiReport(
        kernel_syscalls=len(kernel),
        wrappers=len(wrapper_numbers),
        named_direct_sites=direct_count,
        dispatch_cases=dispatch_count,
    )


def collect_and_validate() -> AbiReport:
    kernel_entries = project_inventory.parse_syscalls(read("syscall.h"))
    wrappers = parse_wrapper_immediates(read("user/user_syscall.h"))
    user_paths = sorted((ROOT / "user").glob("*.c")) + [ROOT / "user" / "crt0.s"]
    direct_sites = parse_named_direct_sites(user_paths)
    dispatch = parse_dispatch_cases(read("syscall.c"))
    return validate(kernel_entries, wrappers, direct_sites, dispatch)


def main() -> int:
    try:
        report = collect_and_validate()
    except (AbiError, project_inventory.InventoryError, OSError) as exc:
        print(str(exc), file=sys.stderr)
        return 1

    print(
        "syscall ABI parity passed: "
        f"{report.wrappers} wrappers + {report.named_direct_sites} named direct sites + "
        f"{report.dispatch_cases} kernel dispatch cases against "
        f"{report.kernel_syscalls} syscall numbers"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
