#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "$0")/.." && pwd)
tmp_dir=$(mktemp -d)
trap 'rm -rf "$tmp_dir"' EXIT

cc=${CC:-gcc}
NATIVE_TEST_SOURCE=tests/test_fd_dup.c

"$cc" -m32 -std=gnu99 -O1 -g -Wall -Wextra -fno-builtin \
    -DHOSTED_TEST -ffunction-sections -fdata-sections \
    -Wl,--gc-sections \
    "$repo_dir/$NATIVE_TEST_SOURCE" -o "$tmp_dir/test_fd_dup"

"$tmp_dir/test_fd_dup"
