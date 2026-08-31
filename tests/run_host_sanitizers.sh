#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo_dir"

# Tests that map fixed low virtual addresses or deliberately recover from
# SIGSEGV are kept on their existing trap-based sanitizer path.  The remaining
# hosted suites can safely run under both ASan and UBSan.
sanitizer_bins=(
    tests/test_utils
    tests/test_fs_path
    tests/test_fs
    tests/test_pmm
    tests/test_heap
    tests/test_fat16
    tests/test_diskfs
    tests/test_pipe
    tests/test_sem
    tests/test_timer
    tests/test_task
    tests/test_rtc
    tests/test_process_env
    tests/test_paging_cow
    tests/test_elf
    tests/test_ramfs
    tests/test_kb
    tests/test_procfs
    tests/test_vga
    tests/test_ata
    tests/test_process
)

cleanup() {
    rm -f "${sanitizer_bins[@]}"
}
trap cleanup EXIT HUP INT TERM

cleanup
sanitizer_cflags="-m32 -std=gnu99 -O1 -g -Wall -Wextra -fno-builtin -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all"
make UNIT_CFLAGS="$sanitizer_cflags" "${sanitizer_bins[@]}"

export ASAN_OPTIONS="detect_leaks=0:halt_on_error=1:strict_string_checks=1"
export UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1"
for test_bin in "${sanitizer_bins[@]}"; do
    "$test_bin"
done

echo "host ASan/UBSan suites passed"
