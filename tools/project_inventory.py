#!/usr/bin/env python3
"""Generate and validate miniOS's source-derived project inventory."""

from __future__ import annotations

import argparse
import difflib
import json
from dataclasses import dataclass
from pathlib import Path
import re
import sys
from typing import Iterable

ROOT = Path(__file__).resolve().parents[1]


class InventoryError(RuntimeError):
    """Raised when project metadata disagrees with the source tree."""


@dataclass(frozen=True)
class Inventory:
    syscalls: tuple[tuple[int, str], ...]
    user_programs: tuple[str, ...]
    native_suites: tuple[str, ...]
    standalone_native_suites: tuple[str, ...]
    qemu_targets: tuple[str, ...]
    stress_mutations: int

    def to_jsonable(self) -> dict[str, object]:
        return {
            "counts": {
                "syscalls": len(self.syscalls),
                "user_programs": len(self.user_programs),
                "native_suites": len(self.native_suites),
                "standalone_native_suites": len(self.standalone_native_suites),
                "qemu_targets": len(self.qemu_targets),
                "stress_mutations": self.stress_mutations,
            },
            "syscalls": [
                {"number": number, "name": name} for number, name in self.syscalls
            ],
            "user_programs": list(self.user_programs),
            "native_suites": list(self.native_suites),
            "standalone_native_suites": list(self.standalone_native_suites),
            "qemu_targets": list(self.qemu_targets),
        }


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def make_logical_lines(text: str) -> list[str]:
    """Join backslash-continued Makefile lines without interpreting make syntax."""
    result: list[str] = []
    pending = ""
    for raw in text.splitlines():
        line = raw.rstrip()
        if pending:
            line = pending + line.lstrip()
        if line.endswith("\\"):
            pending = line[:-1] + " "
            continue
        result.append(line)
        pending = ""
    if pending:
        raise InventoryError("Makefile ends with an unterminated continuation")
    return result


def parse_make_variable(text: str, name: str) -> tuple[str, ...]:
    prefix = re.compile(rf"^\s*{re.escape(name)}\s*=\s*(.*)$")
    for line in make_logical_lines(text):
        match = prefix.match(line)
        if match:
            return tuple(match.group(1).split())
    raise InventoryError(f"missing Makefile variable {name}")


def parse_target_dependencies(text: str, target: str) -> tuple[str, ...]:
    prefix = re.compile(rf"^\s*{re.escape(target)}\s*:\s*(.*)$")
    for line in make_logical_lines(text):
        match = prefix.match(line)
        if match:
            return tuple(match.group(1).split())
    raise InventoryError(f"missing Makefile target {target}")


def parse_syscalls(text: str) -> tuple[tuple[int, str], ...]:
    match = re.search(r"enum\s+syscall_number\s*\{(.*?)\};", text, re.S)
    if not match:
        raise InventoryError("could not find enum syscall_number")

    entries = [
        (int(number), name)
        for name, number in re.findall(
            r"^\s*(SYS_[A-Z0-9_]+)\s*=\s*(\d+)\s*,",
            match.group(1),
            re.M,
        )
    ]
    if not entries:
        raise InventoryError("syscall_number contains no explicit entries")

    numbers = [number for number, _ in entries]
    names = [name for _, name in entries]
    if len(numbers) != len(set(numbers)):
        raise InventoryError("duplicate syscall number")
    if len(names) != len(set(names)):
        raise InventoryError("duplicate syscall name")

    expected = list(range(1, len(entries) + 1))
    if numbers != expected:
        raise InventoryError(
            "syscall ABI must stay contiguous from 1; "
            f"found {numbers}, expected {expected}"
        )
    return tuple(entries)


def parse_executed_shell_scripts(text: str) -> tuple[str, ...]:
    """Return repo-local shell scripts directly executed by a gate script."""
    return tuple(
        re.findall(r"^\s*bash\s+(tests/[A-Za-z0-9_.-]+\.sh)\s*$", text, re.M)
    )


def parse_native_test_targets(text: str) -> tuple[str, ...]:
    """Return the native C test explicitly declared by a standalone runner.

    A bare mention of ``tests/test_*.c`` in a comment or diagnostic is not
    evidence that the runner compiles it. Standalone runners therefore expose a
    small machine-readable contract and use that variable in their compile
    command::

        NATIVE_TEST_SOURCE=tests/test_example.c
        "$cc" ... "$repo_dir/$NATIVE_TEST_SOURCE" ...
    """
    matches = re.findall(
        r"^\s*NATIVE_TEST_SOURCE\s*=\s*(tests/test_[A-Za-z0-9_]+\.c)\s*$",
        text,
        re.M,
    )
    if not matches:
        return ()
    if len(matches) != 1:
        raise InventoryError("standalone runner must declare NATIVE_TEST_SOURCE exactly once")
    if "$NATIVE_TEST_SOURCE" not in text and "${NATIVE_TEST_SOURCE}" not in text:
        raise InventoryError("standalone runner declares but does not use NATIVE_TEST_SOURCE")
    source = matches[0]
    return (source[:-2],)


def program_name_from_embed(token: str) -> str | None:
    suffix = "_embed.o"
    if not token.endswith(suffix):
        return None
    name = token[: -len(suffix)]
    if name == "fat16_image":
        return None
    return name


def registered_programs(kernel: str) -> set[str]:
    pattern = re.compile(
        r'ramfs_create_static_file\(\s*"([^"]+)"\s*,\s*'
        r'([a-zA-Z0-9_]+)_elf_data\s*,\s*\2_elf_size\s*\)'
    )
    matches = pattern.findall(kernel)
    mismatched = [f"{name}->{symbol}" for name, symbol in matches if name != symbol]
    if mismatched:
        raise InventoryError(
            "kernel RAMFS ELF name/symbol mismatch: " + ", ".join(mismatched)
        )
    return {name for name, _symbol in matches}


def require_same(label: str, expected: Iterable[str], actual: Iterable[str]) -> None:
    expected_set = set(expected)
    actual_set = set(actual)
    if expected_set == actual_set:
        return
    missing = sorted(expected_set - actual_set)
    extra = sorted(actual_set - expected_set)
    pieces = []
    if missing:
        pieces.append("missing " + ", ".join(missing))
    if extra:
        pieces.append("extra " + ", ".join(extra))
    raise InventoryError(f"{label}: " + "; ".join(pieces))


def require_substrings(label: str, text: str, needles: Iterable[str]) -> None:
    missing = [needle for needle in needles if needle not in text]
    if missing:
        formatted = ", ".join(repr(item) for item in missing)
        raise InventoryError(f"{label} has stale/missing inventory text: {formatted}")


def collect() -> Inventory:
    root_make = read("Makefile")
    user_make = read("user/Makefile")
    kernel = read("kernel.c")

    syscalls = parse_syscalls(read("syscall.h"))

    program_tokens = parse_make_variable(user_make, "PROGRAMS")
    if any(not token.endswith(".elf") for token in program_tokens):
        raise InventoryError("every user/Makefile PROGRAMS entry must end in .elf")
    user_programs = tuple(token[:-4] for token in program_tokens)
    if len(user_programs) != len(set(user_programs)):
        raise InventoryError("duplicate user program in user/Makefile")

    missing_sources = [
        name for name in user_programs if not (ROOT / "user" / f"{name}.c").is_file()
    ]
    if missing_sources:
        raise InventoryError(
            "PROGRAMS entries without matching C source: " + ", ".join(missing_sources)
        )

    embedded = {
        name
        for token in parse_make_variable(root_make, "OBJS")
        if (name := program_name_from_embed(token)) is not None
    }
    require_same(
        "embedded user-program objects disagree with user/Makefile",
        user_programs,
        embedded,
    )
    require_same(
        "kernel RAMFS registrations disagree with user/Makefile",
        user_programs,
        registered_programs(kernel),
    )

    native_suites = parse_make_variable(root_make, "UNIT_BINS")
    if len(native_suites) != len(set(native_suites)):
        raise InventoryError("duplicate native test suite in UNIT_BINS")
    missing_native_sources = [
        target for target in native_suites if not (ROOT / f"{target}.c").is_file()
    ]
    if missing_native_sources:
        raise InventoryError(
            "UNIT_BINS entries without matching source: "
            + ", ".join(missing_native_sources)
        )

    static_gate = read("tests/run_static_analysis.sh")
    standalone_native: list[str] = []
    for runner in parse_executed_shell_scripts(static_gate):
        runner_path = ROOT / runner
        if not runner_path.is_file():
            raise InventoryError(f"static-analysis executes missing script {runner}")
        for target in parse_native_test_targets(runner_path.read_text(encoding="utf-8")):
            if target not in standalone_native:
                standalone_native.append(target)
    standalone_native_suites = tuple(standalone_native)

    source_native_suites = tuple(
        f"tests/{path.stem}" for path in sorted((ROOT / "tests").glob("test_*.c"))
    )
    require_same(
        "native C test sources disagree with registered unit/standalone gates",
        source_native_suites,
        tuple(native_suites) + standalone_native_suites,
    )

    test_deps = parse_target_dependencies(root_make, "test")
    qemu_targets = tuple(dep for dep in test_deps if dep.startswith("test-"))
    if not qemu_targets:
        raise InventoryError("top-level test target contains no QEMU test-* dependency")
    for target in qemu_targets:
        parse_target_dependencies(root_make, target)

    stress_mutations = len(
        re.findall(
            r"^\s*run_mutant\s+\\?\s*$",
            read("tests/run_qemu_stress_mutants.sh"),
            re.M,
        )
    )
    if stress_mutations == 0:
        raise InventoryError("no stress mutations detected")

    return Inventory(
        syscalls=syscalls,
        user_programs=user_programs,
        native_suites=native_suites,
        standalone_native_suites=standalone_native_suites,
        qemu_targets=qemu_targets,
        stress_mutations=stress_mutations,
    )


def validate_documented_counts(inv: Inventory) -> None:
    syscall_count = len(inv.syscalls)
    program_count = len(inv.user_programs)
    native_count = len(inv.native_suites)
    qemu_count = len(inv.qemu_targets)

    require_substrings(
        "README.md",
        read("README.md"),
        (
            f"- **{syscall_count} system calls** and **{program_count} user programs / demos**",
            f"- {syscall_count} system calls",
            f"- {program_count} user programs / demos",
        ),
    )
    require_substrings(
        "PROJECT_STATE.md",
        read("PROJECT_STATE.md"),
        (
            f"| 系統呼叫 | {syscall_count} |",
            f"| 使用者程式 | {program_count} |",
            f"| 單元測試套件 | {native_count} |",
        ),
    )
    require_substrings(
        "CLAUDE.md",
        read("CLAUDE.md"),
        (
            f"| `make unit` | {native_count} 套原生單元測試 | <1s |",
            f"| `make test` | 完整回歸：`unit` + {qemu_count} 個 QEMU 目標 | ~8-10 分鐘 |",
        ),
    )


def render_markdown(inv: Inventory) -> str:
    lines = [
        "<!-- Generated by tools/project_inventory.py; do not edit by hand. -->",
        "# miniOS project inventory",
        "",
        "This file is derived from build metadata and kernel registrations. "
        "CI regenerates it and fails if the committed copy drifts.",
        "",
        "| Metric | Count |",
        "|---|---:|",
        f"| System calls | {len(inv.syscalls)} |",
        f"| User programs | {len(inv.user_programs)} |",
        f"| Native unit suites | {len(inv.native_suites)} |",
        f"| Standalone native gates | {len(inv.standalone_native_suites)} |",
        f"| QEMU regression targets in `make test` | {len(inv.qemu_targets)} |",
        f"| QEMU stress mutants | {inv.stress_mutations} |",
        "",
        "## System calls",
        "",
    ]
    lines.extend(f"- `{number:02d}` `{name}`" for number, name in inv.syscalls)
    lines += ["", "## User programs", ""]
    lines.extend(f"- `{name}`" for name in inv.user_programs)
    lines += ["", "## Native unit suites", ""]
    lines.extend(f"- `{target}`" for target in inv.native_suites)
    lines += ["", "## Standalone native gates", ""]
    if inv.standalone_native_suites:
        lines.extend(f"- `{target}`" for target in inv.standalone_native_suites)
    else:
        lines.append("- none")
    lines += ["", "## QEMU regression targets", ""]
    lines.extend(f"- `{target}`" for target in inv.qemu_targets)
    lines += [
        "",
        "## Consistency checks enforced by the generator",
        "",
        "- syscall IDs are explicit, unique, and contiguous from 1",
        "- every `user/Makefile` program has a matching `user/<name>.c` source",
        "- every user program has a matching root-Makefile `*_embed.o` object",
        "- every user program is registered to its matching embedded ELF in `kernel.c`",
        "- every `UNIT_BINS` target has a matching test source",
        "- every `tests/test_*.c` source is registered in `UNIT_BINS` or an executed standalone native gate",
        "- every standalone native gate runner executed by static-analysis exists",
        "- every QEMU dependency of the top-level `test` target is defined",
        "- README / PROJECT_STATE / CLAUDE headline counts match source-derived totals",
        "- the stress mutation harness contains at least one `run_mutant` case",
        "",
        "Regenerate with:",
        "",
        "```sh",
        "python3 tools/project_inventory.py --write docs/PROJECT_INVENTORY.md",
        "```",
        "",
        "Verify without modifying files:",
        "",
        "```sh",
        "python3 tools/project_inventory.py --check docs/PROJECT_INVENTORY.md",
        "```",
        "",
    ]
    return "\n".join(lines)


def check(path: Path, rendered: str) -> int:
    try:
        current = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        print(f"inventory missing: {path}", file=sys.stderr)
        return 1
    if current == rendered:
        print(f"project inventory is up to date: {path}")
        return 0

    print(f"project inventory drift: {path}", file=sys.stderr)
    diff = difflib.unified_diff(
        current.splitlines(),
        rendered.splitlines(),
        fromfile=str(path),
        tofile="generated",
        lineterm="",
    )
    for line in diff:
        print(line, file=sys.stderr)
    return 1


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--check", metavar="PATH", type=Path)
    mode.add_argument("--write", metavar="PATH", type=Path)
    mode.add_argument("--json", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        inventory = collect()
        if args.check:
            validate_documented_counts(inventory)
    except (InventoryError, OSError) as exc:
        print(f"project inventory validation failed: {exc}", file=sys.stderr)
        return 1

    if args.json:
        print(json.dumps(inventory.to_jsonable(), indent=2, sort_keys=True))
        return 0

    rendered = render_markdown(inventory)
    if args.check:
        path = args.check if args.check.is_absolute() else ROOT / args.check
        return check(path, rendered)
    if args.write:
        path = args.write if args.write.is_absolute() else ROOT / args.write
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(rendered, encoding="utf-8")
        print(f"wrote project inventory: {path}")
        return 0

    print(rendered, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
