#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "$0")/.." && pwd)
tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT

cp -a "$repo_dir/user" "$tmp_dir/user"

# Make the freshness proof independent of checkout/build timestamp granularity.
# Some CI filesystems can give freshly checked-out sources mtimes equal to or
# slightly newer than outputs produced in the same clock tick. Put the relevant
# source inputs on a fixed old baseline, build, then normalize the generated
# artifacts to a later fixed baseline before asking make -q about freshness.
touch -t 200001010000 \
    "$tmp_dir/user/hello.c" \
    "$tmp_dir/user/user_syscall.h" \
    "$tmp_dir/user/crt0.s"

make -C "$tmp_dir/user" clean hello.elf >/dev/null

dep_file="$tmp_dir/user/hello.d"
if [[ ! -f "$dep_file" ]]; then
    echo "user dependency regression: hello.d was not generated" >&2
    exit 1
fi
if ! grep -Eq '^hello\.o:.*user_syscall\.h' "$dep_file"; then
    echo "user dependency regression: hello.d does not track user_syscall.h" >&2
    cat "$dep_file" >&2
    exit 1
fi

# 2001 outputs are unambiguously newer than the 2000 source baseline. Equal
# object/ELF mtimes are fine: GNU make rebuilds only when a prerequisite is
# strictly newer than its target.
touch -t 200101010000 \
    "$tmp_dir/user/crt0.o" \
    "$tmp_dir/user/hello.o" \
    "$tmp_dir/user/hello.d" \
    "$tmp_dir/user/hello.elf"

if ! make -C "$tmp_dir/user" -q hello.elf; then
    echo "user dependency regression: clean hello.elf build is unexpectedly stale" >&2
    exit 1
fi

printf '\n/* dependency-regression marker */\n' >> "$tmp_dir/user/user_syscall.h"
# Force the edited header into a third deterministic timestamp epoch so the
# stale transition does not depend on sub-second mtime resolution either.
touch -t 200201010000 "$tmp_dir/user/user_syscall.h"

set +e
make -C "$tmp_dir/user" -q hello.elf >/dev/null 2>&1
query_status=$?
set -e
if [[ $query_status -ne 1 ]]; then
    echo "user dependency regression: header edit did not mark hello.elf stale (make -q=$query_status)" >&2
    exit 1
fi

rebuild_output=$(make -C "$tmp_dir/user" --no-print-directory hello.elf 2>&1)
if ! grep -Fq 'hello.c' <<<"$rebuild_output"; then
    echo "user dependency regression: header edit did not recompile hello.o" >&2
    printf '%s\n' "$rebuild_output" >&2
    exit 1
fi
if ! grep -Fq 'hello.elf' <<<"$rebuild_output"; then
    echo "user dependency regression: rebuilt object did not relink hello.elf" >&2
    printf '%s\n' "$rebuild_output" >&2
    exit 1
fi

if ! make -C "$tmp_dir/user" -q hello.elf; then
    echo "user dependency regression: rebuilt hello.elf remains stale" >&2
    exit 1
fi

# A real file named clean must never suppress the cleanup recipe.
touch "$tmp_dir/user/clean"
make -C "$tmp_dir/user" clean >/dev/null
if [[ -e "$tmp_dir/user/hello.o" || -e "$tmp_dir/user/hello.elf" ]]; then
    echo "user dependency regression: phony clean target did not remove build outputs" >&2
    exit 1
fi
if compgen -G "$tmp_dir/user/*.d" >/dev/null; then
    echo "user dependency regression: make clean left dependency files behind" >&2
    exit 1
fi

echo "user incremental dependency regression passed"
