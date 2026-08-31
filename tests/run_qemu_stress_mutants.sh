#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo_dir"

active_file=
backup_file=
baseline_hash=
driver_log=tests/qemu-stress-driver.log

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

    rm -f tests/qemu-stress.log "$driver_log"
    set +e
    make test-stress 2>&1 | tee "$driver_log"
    rc=${PIPESTATUS[0]}
    set -e

    restore_active

    if [[ $rc -eq 0 ]]; then
        echo "mutant survived: $file" >&2
        exit 1
    fi
    if [[ ! -f "$driver_log" ]] ||
       ! grep -Fq "$expected_marker" "$driver_log"; then
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

run_mutant \
    paging.c \
    $'    /* Free the frame only once its last COW sharer is gone (as in destroy). */\n    if (cow_ref_release(page->frame)) {\n        pmm_free_block((void *)(page->frame << 12));\n    }\n    memset(page, 0, sizeof(*page));' \
    $'    /* Free the frame only once its last COW sharer is gone (as in destroy). */\n    if (cow_ref_release(page->frame)) {\n        /* mutant: drop the mapping without returning its physical frame */\n    }\n    memset(page, 0, sizeof(*page));' \
    'resource snapshot drift:'

run_mutant \
    paging.c \
    '        task_exit(-1);' \
    '        task_exit(-2); /* mutant: corrupt the user-fault exit status */' \
    '[stress fault isolation status FAIL]'

run_mutant \
    process.c \
    $'    syscall_close_user_files(process);\n    paging_destroy_user_address_space(process->address_space);' \
    $'    /* mutant: discard the fd table without closing files or pipes */\n    paging_destroy_user_address_space(process->address_space);' \
    '[stress fault isolation status FAIL]'

run_mutant \
    isr.c \
    '    if ((regs->cs & 0x3) == 0x3) {' \
    '    if ((regs->cs & 0x3) == 0x0) { /* mutant: misclassify user exceptions */' \
    'KERNEL EXCEPTION 0'

run_mutant \
    isr.c \
    '        task_exit(-1);' \
    '        task_exit(-2); /* mutant: corrupt generic exception status */' \
    '[stress fault isolation status FAIL]'

echo "QEMU stress mutations killed (7/7)"
