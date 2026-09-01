#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "$0")/.." && pwd)
tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT

cc=${CC:-gcc}
NATIVE_TEST_SOURCE=tests/test_wait_concurrency.c

"$cc" -m32 -std=gnu99 -O1 -g -Wall -Wextra -fno-builtin \
    -DHOSTED_TEST -ffunction-sections -fdata-sections \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -fno-omit-frame-pointer -Wl,--gc-sections \
    "$repo_dir/$NATIVE_TEST_SOURCE" -o "$tmp_dir/test_wait_concurrency"

ASAN_OPTIONS="detect_leaks=0:halt_on_error=1:strict_string_checks=1" \
UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" \
    "$tmp_dir/test_wait_concurrency"

echo "concurrent waiters ASan/UBSan regression passed"
