#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "$0")/.." && pwd)
tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT

cp -a "$repo_dir/user" "$tmp_dir/user"

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

if ! make -C "$tmp_dir/user" -q hello.elf; then
    echo "user dependency regression: clean hello.elf build is unexpectedly stale" >&2
    exit 1
fi

printf '\n/* dependency-regression marker */\n' >> "$tmp_dir/user/user_syscall.h"

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
