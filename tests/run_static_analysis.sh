#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo_dir"

python3 -m py_compile \
    gen_embed.py gen_fat16.py gen_ata_image.py tests/run_qemu_stress.py \
    tests/apply_mutation.py tools/project_inventory.py tests/test_project_inventory.py \
    tools/check_syscall_abi.py tests/test_syscall_abi.py \
    tools/check_test_gate_registration.py tests/test_test_gate_registration.py \
    tools/check_native_test_uniqueness.py tests/test_native_test_uniqueness.py \
    tools/check_gnu_stack_notes.py tests/test_gnu_stack_notes.py

python3 tests/test_project_inventory.py
python3 tools/project_inventory.py --check docs/PROJECT_INVENTORY.md
python3 tests/test_syscall_abi.py
python3 tools/check_syscall_abi.py
python3 tests/test_test_gate_registration.py
python3 tools/check_test_gate_registration.py
python3 tests/test_native_test_uniqueness.py
python3 tools/check_native_test_uniqueness.py
python3 tests/test_gnu_stack_notes.py
python3 tools/check_gnu_stack_notes.py

bash -n \
    tests/run_host_sanitizers.sh tests/run_qemu_stress_mutants.sh \
    tests/run_static_analysis.sh tests/test_user_incremental_build.sh \
    tests/run_fd_dup_test.sh tests/run_fd_index_ubsan.sh \
    tests/run_wait_concurrency_test.sh

bash tests/test_user_incremental_build.sh
bash tests/run_fd_dup_test.sh
bash tests/run_fd_index_ubsan.sh
bash tests/run_wait_concurrency_test.sh

if ! command -v cppcheck >/dev/null 2>&1; then
    echo "cppcheck is required for static-analysis" >&2
    exit 1
fi

# These are the kernel modules also compiled into hosted unit tests, plus the
# ring-3 stress program and shell that drive the QEMU regressions.  Parsing
# them as 32-bit C with HOSTED_TEST catches portable defects without asking
# cppcheck to model privileged assembly or generated ELF byte arrays.
cppcheck \
    --quiet \
    --std=c99 \
    --platform=unix32 \
    --enable=warning,performance,portability \
    --error-exitcode=1 \
    --suppress=missingIncludeSystem \
    --suppress=checkersReport \
    -DHOSTED_TEST \
    utils.c fs.c pmm.c heap.c ramfs.c diskfs.c fat16.c pipe.c sem.c \
    timer.c task.c rtc.c procfs.c vga.c ata.c isr.c process.c syscall.c elf_loader.c \
    user/fault.c user/stress.c user/ush.c

echo "Python, test registration/ownership, inventory, syscall ABI, GNU-stack metadata, incremental user build, fd dup/index UBSan, concurrent waits, shell, and cppcheck static analysis passed"
