#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo_dir"

NATIVE_TEST_SOURCE=tests/test_fd_index_ubsan.c
bin=tests/test_fd_index_ubsan

cleanup() {
    rm -f "$bin"
}
trap cleanup EXIT HUP INT TERM

cleanup

cflags="-m32 -std=gnu99 -O1 -g -Wall -Wextra -fno-builtin -DHOSTED_TEST -ffunction-sections -fdata-sections -fsanitize=undefined -fno-sanitize-recover=all"

gcc $cflags "$NATIVE_TEST_SOURCE" -Wl,--gc-sections -o "$bin"

export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"
"$bin"

echo "fd index UBSan regression passed"
