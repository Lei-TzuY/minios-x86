#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo_dir"

active_file=
backup_file=
baseline_hash=

restore_active() {
    if [[ -n "$active_file" && -n "$backup_file" && -f "$backup_file" ]]; then
        cp "$backup_file" "$active_file"
        restored_hash=$(sha256sum "$active_file" | awk '{print $1}')
        if [[ "$restored_hash" != "$baseline_hash" ]]; then
            echo "failed to restore $active_file byte-for-byte" >&2
            exit 1
        fi
        touch "$active_file"
        rm -f "$backup_file"
        active_file=
        backup_file=
        baseline_hash=
    fi
}
trap restore_active EXIT HUP INT TERM

run_mutant() {
    local file=$1
    local old=$2
    local new=$3
    local expected_marker=$4
    local rc

    active_file=$file
    backup_file=$(mktemp)
    cp "$active_file" "$backup_file"
    baseline_hash=$(sha256sum "$active_file" | awk '{print $1}')
    python3 tests/apply_mutation.py "$active_file" "$old" "$new"

    rm -f tests/qemu-stress.log
    set +e
    make test-stress
    rc=$?
    set -e

    restore_active

    if [[ $rc -eq 0 ]]; then
        echo "mutant survived: $file" >&2
        exit 1
    fi
    if [[ ! -f tests/qemu-stress.log ]] ||
       ! grep -Fq "$expected_marker" tests/qemu-stress.log; then
        echo "mutant failed for the wrong reason; missing $expected_marker" >&2
        exit 1
    fi
    echo "mutant killed by named assertion: $expected_marker"
}

run_mutant \
    syscall.c \
    '#define MAX_OPEN_FILES 8' \
    '#define MAX_OPEN_FILES 7' \
    '[stress fd exhaustion fill FAIL]'

run_mutant \
    process.c \
    $'static process_t *process_allocate(void) {\n    for (int i = 0; i < MAX_PROCESSES; i++) {' \
    $'static process_t *process_allocate(void) {\n    for (int i = 0; i < MAX_PROCESSES - 1; i++) {' \
    '[stress process exhaustion fill FAIL]'

echo "QEMU stress mutations killed (2/2)"
