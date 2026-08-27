#!/usr/bin/env bash
set -euo pipefail

repo_dir=$(cd "$(dirname "$0")/.." && pwd)
cd "$repo_dir"

python3 -m py_compile \
    gen_embed.py gen_fat16.py gen_ata_image.py tests/run_qemu_stress.py \
    tests/apply_mutation.py

bash -n \
    tests/run_host_sanitizers.sh tests/run_qemu_stress_mutants.sh \
    tests/run_static_analysis.sh

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
    timer.c task.c rtc.c procfs.c vga.c ata.c process.c syscall.c elf_loader.c \
    user/fault.c user/stress.c user/ush.c

echo "Python, shell, and cppcheck static analysis passed"
