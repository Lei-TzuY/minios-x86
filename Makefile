CC   = gcc
AS   = as
QEMU = qemu-system-i386
ATA_IMAGE = ata-test.img
ATA_DRIVE = -drive file=$(ATA_IMAGE),format=raw,if=ide,index=0,media=disk,snapshot=on

ASFLAGS = --32
CFLAGS  = -m32 -std=gnu99 -ffreestanding -fno-pie -fno-stack-protector -O2 -Wall -Wextra
LDFLAGS = -m32 -ffreestanding -fno-pie -no-pie -O2 -nostdlib -Wl,--build-id=none

OBJS = boot.o kernel.o vga.o gdt.o gdt_s.o idt.o isr.o interrupt.o \
       kb.o timer.o utils.o pmm.o paging.o paging_s.o task.o switch_s.o \
       syscall.o usermode_s.o heap.o fs.o ramfs.o procfs.o rtc.o sem.o process.o pipe.o elf_loader.o ata.o diskfs.o fat16.o fat16_image_embed.o hello_embed.o \
       cat_embed.o fault_embed.o badptr_embed.o worker_embed.o spawner_embed.o \
       orphan_embed.o sleeptest_embed.o fstest_embed.o echo_embed.o \
       malloctest_embed.o wc_embed.o grep_embed.o \
       head_embed.o tail_embed.o sort_embed.o sigtest_embed.o sigipc_embed.o forktest_embed.o execdemo_embed.o demandtest_embed.o sigchld_embed.o waitdemo_embed.o cwddemo_embed.o statdemo_embed.o cowstress_embed.o alarmdemo_embed.o pausedemo_embed.o pipedemo_embed.o jobctl_embed.o uptime_embed.o date_embed.o printenv_embed.o cputime_embed.o shmtest_embed.o semtest_embed.o mmaptest_embed.o threadtest_embed.o threadexit_embed.o execguard_embed.o ramgrow_embed.o pathlim_embed.o redirref_embed.o fatref_embed.o fatgrow_embed.o forkredir_embed.o sigretguard_embed.o sigflags_embed.o killthread_embed.o killwait_embed.o bigseek_embed.o stress_embed.o ush_embed.o

.PHONY: all clean run run-headless iso run-iso test test-ata-absent test-boot \
        test-iso test-shell test-stress test-stress-mutants unit sanitize \
        static-analysis bench

# --- Native unit tests -------------------------------------------------------
# Kernel modules that are pure logic are compiled for the host and called
# directly. They finish in well under a second, so a logic regression surfaces
# immediately instead of after the multi-minute QEMU run, and they can probe
# edge cases the shell cannot reach at all (every buffer alignment, the exact
# path-length boundary, a heap coalescing decision).
#
# -m32 matches the kernel's pointer size, which matters for the allocators.
# -fno-builtin stops gcc from turning our own memcpy/memset bodies into calls
# to themselves, and from replacing the calls under test with its builtins.
UNIT_CFLAGS = -m32 -std=gnu99 -O1 -g -Wall -Wextra -fno-builtin
UNIT_BINS = tests/test_utils tests/test_fs_path tests/test_fs tests/test_pmm \
            tests/test_heap \
            tests/test_fat16 tests/test_diskfs tests/test_pipe tests/test_sem \
            tests/test_timer tests/test_task tests/test_rtc \
            tests/test_process_env tests/test_syscall_valid \
            tests/test_paging_cow tests/test_elf tests/test_ramfs \
            tests/test_kb tests/test_procfs tests/test_vga tests/test_ata \
            tests/test_fdtable tests/test_process tests/test_signal \
            tests/test_vm_lifecycle

tests/test_utils: tests/test_utils.c tests/test.h utils.c utils.h
	$(CC) $(UNIT_CFLAGS) tests/test_utils.c utils.c -o $@

# ramfs.c needs only the allocator (stubbed in the test) and fs_root, so fs.c is
# not linked. The real utils.c is used so the memcpy/memset the filesystem
# actually ships is what moves the data.
tests/test_ramfs: tests/test_ramfs.c tests/test.h tests/fs_conformance.h \
                  ramfs.c ramfs.h fs.h heap.h utils.c utils.h
	$(CC) $(UNIT_CFLAGS) tests/test_ramfs.c ramfs.c utils.c -o $@

tests/test_fs_path: tests/test_fs_path.c tests/test.h fs.c fs.h utils.c utils.h
	$(CC) $(UNIT_CFLAGS) tests/test_fs_path.c fs.c utils.c -o $@

# The VFS core: the strict resolvers and the dispatch wrappers, driven against
# a mock filesystem the test builds itself. No real backend is linked -- the
# mock is deliberately more permissive than any of them (it will return a child
# literally named "."), so every rejection the test observes is fs.c's own.
tests/test_fs: tests/test_fs.c tests/test.h fs.c fs.h utils.c utils.h
	$(CC) $(UNIT_CFLAGS) tests/test_fs.c fs.c utils.c -o $@

tests/test_pmm: tests/test_pmm.c tests/test.h pmm.c pmm.h
	$(CC) $(UNIT_CFLAGS) tests/test_pmm.c pmm.c -o $@

tests/test_heap: tests/test_heap.c tests/test.h heap.c heap.h pmm.h
	$(CC) $(UNIT_CFLAGS) tests/test_heap.c heap.c -o $@

# Links the same embedded image the kernel mounts, so the tests run against
# the real generated filesystem rather than a hand-built approximation.
tests/test_fat16: tests/test_fat16.c tests/test.h tests/fs_conformance.h \
                  fat16.c fat16.h fs.c fs.h utils.c utils.h fat16_image_embed.c
	$(CC) $(UNIT_CFLAGS) tests/test_fat16.c fat16.c fs.c utils.c \
	    fat16_image_embed.c -o $@

# ATA is stubbed with a RAM array by the test itself, so ata.c is not linked:
# that is what lets the test hand diskfs a deliberately corrupt disk.
tests/test_diskfs: tests/test_diskfs.c tests/test.h tests/fs_conformance.h \
                   diskfs.c diskfs.h fs.c fs.h utils.c utils.h ata.h
	$(CC) $(UNIT_CFLAGS) tests/test_diskfs.c diskfs.c fs.c utils.c -o $@

# pipe.c and sem.c guard their cli/sti behind HOSTED_TEST so the privileged
# instructions compile out for these ring-3 tests (see pipe.c). The scheduler
# and allocator they call are stubbed inside the test.
tests/test_pipe: tests/test_pipe.c tests/test.h pipe.c pipe.h task.h
	$(CC) $(UNIT_CFLAGS) -DHOSTED_TEST tests/test_pipe.c pipe.c -o $@

tests/test_sem: tests/test_sem.c tests/test.h sem.c sem.h task.h
	$(CC) $(UNIT_CFLAGS) -DHOSTED_TEST tests/test_sem.c sem.c -o $@

# timer.c reaches port I/O (timer_install) and the scheduler; the port ops are
# no-ops under HOSTED_TEST and the scheduler/process hooks are stubbed in the
# test. The static timer_callback is captured via register_interrupt_handler.
tests/test_timer: tests/test_timer.c tests/test.h timer.c timer.h isr.h io.h irq.h task.h
	$(CC) $(UNIT_CFLAGS) -DHOSTED_TEST tests/test_timer.c timer.c -o $@

# The context switch (switch_task, assembly) and paging/pmm are stubbed in the
# test, leaving the ready-ring and blocked-list logic exercisable. task.c's raw
# cli lives in task_exit, which the test never calls.
tests/test_task: tests/test_task.c tests/test.h task.c task.h pmm.h irq.h
	$(CC) $(UNIT_CFLAGS) -DHOSTED_TEST tests/test_task.c task.c -o $@

# procfs.c is included directly so the tests can reach the generators and the
# shared render buffer. The three things it calls out to (the process
# snapshot, the current process, the tick count) are stubbed by the test, so
# neither process.c nor timer.c is linked. The bounds sanitizer in trap mode is
# the second net under the buffer invariant: one generator's guard has never
# executed in a real boot and two others have no guard at all, so a write past
# gen_buf must fault rather than be inferred from a length.
tests/test_procfs: tests/test_procfs.c tests/test.h procfs.c procfs.h fs.h \
                   process.h timer.h utils.c utils.h
	$(CC) $(UNIT_CFLAGS) -fsanitize=undefined,bounds \
	    -fsanitize-undefined-trap-on-error tests/test_procfs.c utils.c -o $@

# vga.c is included directly to reach terminal_scroll and the cursor globals.
# io.h is replaced by the test rather than compiled out, for two reasons: the
# port-0xE9 write is the byte stream the QEMU suite greps, so it is worth
# asserting on, and terminal_buffer can then be pointed at an ordinary array
# instead of the hardware address. terminal_initialize() is the one function
# that cannot run hosted -- its body is the 0xB8000 assignment.
tests/test_vga: tests/test_vga.c tests/test.h vga.c vga.h io.h utils.h
	$(CC) $(UNIT_CFLAGS) tests/test_vga.c -o $@

# ata.c is included directly so the tests reach the polling helpers and the
# driver's static state. BOTH io.h and irq.h are replaced by the test rather
# than compiled out: io.h becomes a fake IDE drive (a state machine that keeps
# the real BSY/DRQ handshake, ignores command-block writes while busy, and
# drops DRQ after exactly 256 words), and irq.h becomes a counting pair so the
# eight return paths through this driver can be checked for a missing restore
# -- which would otherwise leave interrupts off with nothing to notice.
tests/test_ata: tests/test_ata.c tests/test.h ata.c ata.h io.h irq.h
	$(CC) $(UNIT_CFLAGS) tests/test_ata.c -o $@

# The per-process descriptor table. Same --gc-sections trick as
# test_syscall_valid, but aimed at the ownership paths instead of the pointer
# checks: open_fs/close_fs and the four pipe reference calls are stubbed with
# COUNTING versions, so the tests can assert who owns what rather than only
# what a syscall returned. A close with no matching open is recorded as an
# underflow, which is what makes a double release visible at all.
tests/test_fdtable: tests/test_fdtable.c tests/test.h syscall.c syscall.h \
                    process.h fs.h pipe.h paging.h
	$(CC) $(UNIT_CFLAGS) -DHOSTED_TEST -ffunction-sections -fdata-sections \
	    -Wl,--gc-sections tests/test_fdtable.c -o $@

# The process lifecycle state machine. process.c is included directly to reach
# its statics and the three internal exit paths; the scheduler is MODELLED
# rather than stubbed away, because every question here is about who blocked,
# on which channel, and who woke them. task_block_current records the sleeper,
# runs the scripted event it is waiting for, and then checks whether anything
# actually aimed a wake at it -- which is what turns "this would hang forever"
# into an assertion. Address spaces count their own destroy/activate so
# teardown ordering is checkable.
tests/test_process: tests/test_process.c tests/test.h process.c process.h \
                    task.h paging.h fs.h pipe.h syscall.h elf_loader.h
	$(CC) $(UNIT_CFLAGS) -DHOSTED_TEST tests/test_process.c -o $@

# Signal delivery and return, driven against a REAL mapped user stack so the
# frame is built and read back at genuine addresses. --gc-sections keeps the
# stub surface to the scheduler calls these two paths actually reach.
tests/test_signal: tests/test_signal.c tests/test.h syscall.c syscall.h \
                   process.h task.h elf_loader.h isr.h
	$(CC) $(UNIT_CFLAGS) -DHOSTED_TEST -ffunction-sections -fdata-sections \
	    -Wl,--gc-sections tests/test_signal.c -o $@

# mmap/munmap couples a per-process reservation bitmap to page-table teardown.
# This model deliberately schedules a sibling precisely when interrupts first
# re-enable, so an early bitmap release cannot hide behind a serial host run.
tests/test_vm_lifecycle: tests/test_vm_lifecycle.c tests/test.h process.c \
                         syscall.c process.h task.h paging.h irq.h elf_loader.h
	$(CC) $(UNIT_CFLAGS) -DHOSTED_TEST -ffunction-sections -fdata-sections \
	    -Wl,--gc-sections tests/test_vm_lifecycle.c -o $@

# Includes kb.c directly to reach its statics (the ring indices, the modifier
# flags) and the handler handed to register_interrupt_handler. io.h is replaced
# by the test rather than compiled out: the hosted inb() returns a constant 0,
# which would leave a scancode-driven driver with no way to receive input.
tests/test_kb: tests/test_kb.c tests/test.h kb.c kb.h isr.h io.h irq.h task.h process.h vga.h
	$(CC) $(UNIT_CFLAGS) -DHOSTED_TEST tests/test_kb.c -o $@

# Includes rtc.c directly to reach the static rtc_decode; the CMOS port I/O is
# compiled out by HOSTED_TEST. No separate rtc.c object is linked.
tests/test_rtc: tests/test_rtc.c tests/test.h rtc.c rtc.h io.h
	$(CC) $(UNIT_CFLAGS) -DHOSTED_TEST tests/test_rtc.c -o $@

# process.c is large and coupled, but the env functions only reach
# process_get_current -> task_get_current. Including the .c and linking with
# --gc-sections drops every unreferenced function (fork, exec, signals, ...) so
# only task_get_current and the string helpers (stubbed in the test) are needed.
tests/test_process_env: tests/test_process_env.c tests/test.h process.c process.h task.h
	$(CC) $(UNIT_CFLAGS) -DHOSTED_TEST -ffunction-sections -fdata-sections \
	    -Wl,--gc-sections tests/test_process_env.c -o $@

# Same --gc-sections trick as process_env: only the user-pointer validators are
# reached, so paging_user_range_mapped (stubbed) is the sole dependency.
tests/test_syscall_valid: tests/test_syscall_valid.c tests/test.h syscall.c syscall.h paging.h
	$(CC) $(UNIT_CFLAGS) -DHOSTED_TEST -ffunction-sections -fdata-sections \
	    -Wl,--gc-sections tests/test_syscall_valid.c -o $@

# Same --gc-sections trick: only user_pte and the COW refcount helpers are
# reached, so the fault handler, the asm and pmm/heap are all dropped and no
# stub is needed. The bounds sanitizer (trap mode, no libubsan needed) turns an
# out-of-bounds page_ref[] write -- a frame-index off-by-one has no functional
# signature otherwise -- into a hard trap, so such a regression is caught.
tests/test_paging_cow: tests/test_paging_cow.c tests/test.h paging.c paging.h elf_loader.h pmm.h
	$(CC) $(UNIT_CFLAGS) -DHOSTED_TEST -fsanitize=undefined,bounds \
	    -fsanitize-undefined-trap-on-error -ffunction-sections -fdata-sections \
	    -Wl,--gc-sections tests/test_paging_cow.c -o $@

# The loader parses an untrusted file, so the test feeds it deliberately
# malformed images. --gc-sections drops elf_spawn/elf_exec, and with them the
# process_launch/process_wait dependencies, since only elf_load_image is called.
tests/test_elf: tests/test_elf.c tests/test.h elf_loader.c elf_loader.h fs.h paging.h
	$(CC) $(UNIT_CFLAGS) -DHOSTED_TEST -ffunction-sections -fdata-sections \
	    -Wl,--gc-sections tests/test_elf.c -o $@

unit: $(UNIT_BINS)
	@fail=0; \
	for t in $(UNIT_BINS); do ./$$t || fail=1; done; \
	if [ $$fail -ne 0 ]; then echo "unit tests FAILED"; exit 1; fi; \
	echo "unit tests passed"

# Hosted kernel components get a second build under ASan+UBSan.  Fixed-address
# and intentional-fault harnesses remain on their existing trap sanitizer path;
# see the script for the explicit, documented selection.
sanitize: tests/run_host_sanitizers.sh
	bash tests/run_host_sanitizers.sh

# Static analysis is a required CI gate, not an informational report.
static-analysis: tests/run_static_analysis.sh tests/apply_mutation.py
	bash tests/run_static_analysis.sh

# --- Performance measurement -------------------------------------------------
# Informational, not a pass/fail gate: timing is noisy and machine-dependent,
# so `bench` is kept out of `make test`. It exists to back the two performance
# changes (word-at-a-time memcpy/memset, geometric RAMFS growth) with actual
# numbers instead of an unmeasured claim. bench_mem links the real utils.c so
# it times the code the kernel ships; bench_ramfs counts algorithmic work
# (reallocations and bytes copied), which is exact and host-independent.
BENCH_BINS = tests/bench_mem tests/bench_ramfs

tests/bench_mem: tests/bench_mem.c utils.c utils.h
	$(CC) -m32 -O1 -g -Wall -Wextra -fno-builtin tests/bench_mem.c utils.c -o $@

tests/bench_ramfs: tests/bench_ramfs.c ramfs.c ramfs.h fs.h
	$(CC) -m32 -O1 -g -Wall -Wextra -fno-builtin tests/bench_ramfs.c ramfs.c -o $@

bench: $(BENCH_BINS)
	@echo "=== memcpy / memset ==="; ./tests/bench_mem; \
	echo; echo "=== RAMFS growth ==="; ./tests/bench_ramfs

all: kernel.bin

# Assembly objects
boot.o: boot.s
	$(AS) $(ASFLAGS) boot.s -o boot.o

gdt_s.o: gdt_s.s
	$(AS) $(ASFLAGS) gdt_s.s -o gdt_s.o

interrupt.o: interrupt.s
	$(AS) $(ASFLAGS) interrupt.s -o interrupt.o

paging_s.o: paging_s.s
	$(AS) $(ASFLAGS) paging_s.s -o paging_s.o

switch_s.o: switch_s.s
	$(AS) $(ASFLAGS) switch_s.s -o switch_s.o

usermode_s.o: usermode_s.s
	$(AS) $(ASFLAGS) usermode_s.s -o usermode_s.o

# C objects
%.o: %.c
	$(CC) -c $< -o $@ $(CFLAGS)

# User-space programs are built separately and embedded into the kernel.
user/hello.elf: user/hello.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user hello.elf

user/cat.elf: user/cat.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user cat.elf

user/fault.elf: user/fault.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user fault.elf

user/badptr.elf: user/badptr.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user badptr.elf

user/worker.elf: user/worker.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user worker.elf

user/spawner.elf: user/spawner.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user spawner.elf

user/orphan.elf: user/orphan.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user orphan.elf

user/sleeptest.elf: user/sleeptest.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user sleeptest.elf

user/fstest.elf: user/fstest.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user fstest.elf

user/echo.elf: user/echo.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user echo.elf

user/malloctest.elf: user/malloctest.c user/user_syscall.h user/umalloc.h user/crt0.s user/Makefile
	$(MAKE) -C user malloctest.elf

user/wc.elf: user/wc.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user wc.elf

user/grep.elf: user/grep.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user grep.elf

user/head.elf: user/head.c user/user_syscall.h user/ulib.h user/crt0.s user/Makefile
	$(MAKE) -C user head.elf

user/tail.elf: user/tail.c user/user_syscall.h user/umalloc.h user/ulib.h user/crt0.s user/Makefile
	$(MAKE) -C user tail.elf

user/sort.elf: user/sort.c user/user_syscall.h user/umalloc.h user/ulib.h user/crt0.s user/Makefile
	$(MAKE) -C user sort.elf

user/sigtest.elf: user/sigtest.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user sigtest.elf

user/sigipc.elf: user/sigipc.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user sigipc.elf

user/forktest.elf: user/forktest.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user forktest.elf

user/execdemo.elf: user/execdemo.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user execdemo.elf

user/demandtest.elf: user/demandtest.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user demandtest.elf

user/sigchld.elf: user/sigchld.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user sigchld.elf

user/waitdemo.elf: user/waitdemo.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user waitdemo.elf

user/cwddemo.elf: user/cwddemo.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user cwddemo.elf

user/statdemo.elf: user/statdemo.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user statdemo.elf

user/cowstress.elf: user/cowstress.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user cowstress.elf

user/alarmdemo.elf: user/alarmdemo.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user alarmdemo.elf

user/pausedemo.elf: user/pausedemo.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user pausedemo.elf

user/pipedemo.elf: user/pipedemo.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user pipedemo.elf

user/jobctl.elf: user/jobctl.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user jobctl.elf

user/uptime.elf: user/uptime.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user uptime.elf

user/date.elf: user/date.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user date.elf

user/printenv.elf: user/printenv.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user printenv.elf

user/cputime.elf: user/cputime.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user cputime.elf

user/shmtest.elf: user/shmtest.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user shmtest.elf

user/semtest.elf: user/semtest.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user semtest.elf

user/mmaptest.elf: user/mmaptest.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user mmaptest.elf

user/threadtest.elf: user/threadtest.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user threadtest.elf

user/threadexit.elf: user/threadexit.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user threadexit.elf

user/execguard.elf: user/execguard.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user execguard.elf

user/ramgrow.elf: user/ramgrow.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user ramgrow.elf

user/pathlim.elf: user/pathlim.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user pathlim.elf

user/redirref.elf: user/redirref.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user redirref.elf

user/fatref.elf: user/fatref.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user fatref.elf

user/fatgrow.elf: user/fatgrow.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user fatgrow.elf

user/forkredir.elf: user/forkredir.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user forkredir.elf

user/sigflags.elf: user/sigflags.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user sigflags.elf

user/sigretguard.elf: user/sigretguard.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user sigretguard.elf

user/killthread.elf: user/killthread.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user killthread.elf

user/killwait.elf: user/killwait.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user killwait.elf

user/bigseek.elf: user/bigseek.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user bigseek.elf

user/stress.elf: user/stress.c user/user_syscall.h user/umalloc.h user/crt0.s user/Makefile
	$(MAKE) -C user stress.elf

user/ush.elf: user/ush.c user/user_syscall.h user/crt0.s user/Makefile
	$(MAKE) -C user ush.elf

hello_embed.c: user/hello.elf gen_embed.py
	python3 gen_embed.py user/hello.elf hello_elf > $@

hello_embed.o: hello_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

cat_embed.c: user/cat.elf gen_embed.py
	python3 gen_embed.py user/cat.elf cat_elf > $@

cat_embed.o: cat_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

fault_embed.c: user/fault.elf gen_embed.py
	python3 gen_embed.py user/fault.elf fault_elf > $@

fault_embed.o: fault_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

badptr_embed.c: user/badptr.elf gen_embed.py
	python3 gen_embed.py user/badptr.elf badptr_elf > $@

badptr_embed.o: badptr_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

worker_embed.c: user/worker.elf gen_embed.py
	python3 gen_embed.py user/worker.elf worker_elf > $@

worker_embed.o: worker_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

spawner_embed.c: user/spawner.elf gen_embed.py
	python3 gen_embed.py user/spawner.elf spawner_elf > $@

spawner_embed.o: spawner_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

orphan_embed.c: user/orphan.elf gen_embed.py
	python3 gen_embed.py user/orphan.elf orphan_elf > $@

orphan_embed.o: orphan_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

sleeptest_embed.c: user/sleeptest.elf gen_embed.py
	python3 gen_embed.py user/sleeptest.elf sleeptest_elf > $@

sleeptest_embed.o: sleeptest_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

fstest_embed.c: user/fstest.elf gen_embed.py
	python3 gen_embed.py user/fstest.elf fstest_elf > $@

fstest_embed.o: fstest_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

echo_embed.c: user/echo.elf gen_embed.py
	python3 gen_embed.py user/echo.elf echo_elf > $@

echo_embed.o: echo_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

malloctest_embed.c: user/malloctest.elf gen_embed.py
	python3 gen_embed.py user/malloctest.elf malloctest_elf > $@

malloctest_embed.o: malloctest_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

wc_embed.c: user/wc.elf gen_embed.py
	python3 gen_embed.py user/wc.elf wc_elf > $@

wc_embed.o: wc_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

grep_embed.c: user/grep.elf gen_embed.py
	python3 gen_embed.py user/grep.elf grep_elf > $@

grep_embed.o: grep_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

head_embed.c: user/head.elf gen_embed.py
	python3 gen_embed.py user/head.elf head_elf > $@

head_embed.o: head_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

tail_embed.c: user/tail.elf gen_embed.py
	python3 gen_embed.py user/tail.elf tail_elf > $@

tail_embed.o: tail_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

sort_embed.c: user/sort.elf gen_embed.py
	python3 gen_embed.py user/sort.elf sort_elf > $@

sort_embed.o: sort_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

sigtest_embed.c: user/sigtest.elf gen_embed.py
	python3 gen_embed.py user/sigtest.elf sigtest_elf > $@

sigtest_embed.o: sigtest_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

sigipc_embed.c: user/sigipc.elf gen_embed.py
	python3 gen_embed.py user/sigipc.elf sigipc_elf > $@

sigipc_embed.o: sigipc_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

forktest_embed.c: user/forktest.elf gen_embed.py
	python3 gen_embed.py user/forktest.elf forktest_elf > $@

forktest_embed.o: forktest_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

execdemo_embed.c: user/execdemo.elf gen_embed.py
	python3 gen_embed.py user/execdemo.elf execdemo_elf > $@

execdemo_embed.o: execdemo_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

demandtest_embed.c: user/demandtest.elf gen_embed.py
	python3 gen_embed.py user/demandtest.elf demandtest_elf > $@

demandtest_embed.o: demandtest_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

sigchld_embed.c: user/sigchld.elf gen_embed.py
	python3 gen_embed.py user/sigchld.elf sigchld_elf > $@

sigchld_embed.o: sigchld_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

waitdemo_embed.c: user/waitdemo.elf gen_embed.py
	python3 gen_embed.py user/waitdemo.elf waitdemo_elf > $@

waitdemo_embed.o: waitdemo_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

cwddemo_embed.c: user/cwddemo.elf gen_embed.py
	python3 gen_embed.py user/cwddemo.elf cwddemo_elf > $@

cwddemo_embed.o: cwddemo_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

statdemo_embed.c: user/statdemo.elf gen_embed.py
	python3 gen_embed.py user/statdemo.elf statdemo_elf > $@

statdemo_embed.o: statdemo_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

cowstress_embed.c: user/cowstress.elf gen_embed.py
	python3 gen_embed.py user/cowstress.elf cowstress_elf > $@

cowstress_embed.o: cowstress_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

alarmdemo_embed.c: user/alarmdemo.elf gen_embed.py
	python3 gen_embed.py user/alarmdemo.elf alarmdemo_elf > $@

alarmdemo_embed.o: alarmdemo_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

pausedemo_embed.c: user/pausedemo.elf gen_embed.py
	python3 gen_embed.py user/pausedemo.elf pausedemo_elf > $@

pausedemo_embed.o: pausedemo_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

pipedemo_embed.c: user/pipedemo.elf gen_embed.py
	python3 gen_embed.py user/pipedemo.elf pipedemo_elf > $@

pipedemo_embed.o: pipedemo_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

jobctl_embed.c: user/jobctl.elf gen_embed.py
	python3 gen_embed.py user/jobctl.elf jobctl_elf > $@

jobctl_embed.o: jobctl_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

uptime_embed.c: user/uptime.elf gen_embed.py
	python3 gen_embed.py user/uptime.elf uptime_elf > $@

uptime_embed.o: uptime_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

date_embed.c: user/date.elf gen_embed.py
	python3 gen_embed.py user/date.elf date_elf > $@

date_embed.o: date_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

printenv_embed.c: user/printenv.elf gen_embed.py
	python3 gen_embed.py user/printenv.elf printenv_elf > $@

printenv_embed.o: printenv_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

cputime_embed.c: user/cputime.elf gen_embed.py
	python3 gen_embed.py user/cputime.elf cputime_elf > $@

cputime_embed.o: cputime_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

shmtest_embed.c: user/shmtest.elf gen_embed.py
	python3 gen_embed.py user/shmtest.elf shmtest_elf > $@

shmtest_embed.o: shmtest_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

semtest_embed.c: user/semtest.elf gen_embed.py
	python3 gen_embed.py user/semtest.elf semtest_elf > $@

semtest_embed.o: semtest_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

mmaptest_embed.c: user/mmaptest.elf gen_embed.py
	python3 gen_embed.py user/mmaptest.elf mmaptest_elf > $@

mmaptest_embed.o: mmaptest_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

threadtest_embed.c: user/threadtest.elf gen_embed.py
	python3 gen_embed.py user/threadtest.elf threadtest_elf > $@

threadtest_embed.o: threadtest_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

threadexit_embed.c: user/threadexit.elf gen_embed.py
	python3 gen_embed.py user/threadexit.elf threadexit_elf > $@

threadexit_embed.o: threadexit_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

execguard_embed.c: user/execguard.elf gen_embed.py
	python3 gen_embed.py user/execguard.elf execguard_elf > $@

execguard_embed.o: execguard_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

ramgrow_embed.c: user/ramgrow.elf gen_embed.py
	python3 gen_embed.py user/ramgrow.elf ramgrow_elf > $@

ramgrow_embed.o: ramgrow_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

pathlim_embed.c: user/pathlim.elf gen_embed.py
	python3 gen_embed.py user/pathlim.elf pathlim_elf > $@

pathlim_embed.o: pathlim_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

redirref_embed.c: user/redirref.elf gen_embed.py
	python3 gen_embed.py user/redirref.elf redirref_elf > $@

redirref_embed.o: redirref_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

fatref_embed.c: user/fatref.elf gen_embed.py
	python3 gen_embed.py user/fatref.elf fatref_elf > $@

fatref_embed.o: fatref_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

fatgrow_embed.c: user/fatgrow.elf gen_embed.py
	python3 gen_embed.py user/fatgrow.elf fatgrow_elf > $@

fatgrow_embed.o: fatgrow_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

forkredir_embed.c: user/forkredir.elf gen_embed.py
	python3 gen_embed.py user/forkredir.elf forkredir_elf > $@

forkredir_embed.o: forkredir_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

sigflags_embed.c: user/sigflags.elf gen_embed.py
	python3 gen_embed.py user/sigflags.elf sigflags_elf > $@

sigflags_embed.o: sigflags_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

sigretguard_embed.c: user/sigretguard.elf gen_embed.py
	python3 gen_embed.py user/sigretguard.elf sigretguard_elf > $@

sigretguard_embed.o: sigretguard_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

killthread_embed.c: user/killthread.elf gen_embed.py
	python3 gen_embed.py user/killthread.elf killthread_elf > $@

killthread_embed.o: killthread_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

killwait_embed.c: user/killwait.elf gen_embed.py
	python3 gen_embed.py user/killwait.elf killwait_elf > $@

killwait_embed.o: killwait_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

bigseek_embed.c: user/bigseek.elf gen_embed.py
	python3 gen_embed.py user/bigseek.elf bigseek_elf > $@

bigseek_embed.o: bigseek_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

stress_embed.c: user/stress.elf gen_embed.py
	python3 gen_embed.py user/stress.elf stress_elf > $@

stress_embed.o: stress_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

ush_embed.c: user/ush.elf gen_embed.py
	python3 gen_embed.py user/ush.elf ush_elf > $@

ush_embed.o: ush_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

# Embedded read-only FAT16 image mounted at /fat.
fat16.img: gen_fat16.py
	python3 gen_fat16.py $@

fat16_image_embed.c: fat16.img gen_embed.py
	python3 gen_embed.py fat16.img fat16_image > $@

fat16_image_embed.o: fat16_image_embed.c
	$(CC) -c $< -o $@ $(CFLAGS)

# Kernel binary
kernel.bin: $(OBJS) linker.ld
	$(CC) -T linker.ld -o kernel.bin $(LDFLAGS) $(OBJS) -lgcc

$(ATA_IMAGE): gen_ata_image.py
	python3 gen_ata_image.py $@

# Utility targets
clean:
	rm -f $(OBJS) kernel.bin $(ATA_IMAGE) hello_embed.c cat_embed.c fault_embed.c badptr_embed.c worker_embed.c spawner_embed.c orphan_embed.c sleeptest_embed.c fstest_embed.c echo_embed.c malloctest_embed.c wc_embed.c grep_embed.c head_embed.c tail_embed.c sort_embed.c sigtest_embed.c sigipc_embed.c forktest_embed.c execdemo_embed.c demandtest_embed.c sigchld_embed.c waitdemo_embed.c cwddemo_embed.c statdemo_embed.c cowstress_embed.c alarmdemo_embed.c pausedemo_embed.c pipedemo_embed.c jobctl_embed.c uptime_embed.c date_embed.c printenv_embed.c cputime_embed.c shmtest_embed.c semtest_embed.c mmaptest_embed.c threadtest_embed.c threadexit_embed.c execguard_embed.c ramgrow_embed.c pathlim_embed.c redirref_embed.c fatref_embed.c fatgrow_embed.c forkredir_embed.c sigretguard_embed.c sigflags_embed.c killthread_embed.c killwait_embed.c bigseek_embed.c stress_embed.c ush_embed.c fat16.img fat16_image_embed.c
	rm -rf isodir miniOS.iso
	rm -f $(UNIT_BINS) $(BENCH_BINS)
	$(MAKE) -C user clean

run: kernel.bin $(ATA_IMAGE)
	$(QEMU) $(ATA_DRIVE) -kernel kernel.bin

# Build a GRUB-bootable ISO so the kernel can run on a VM (VirtualBox/VMware/
# QEMU) or real hardware, not just via QEMU's -kernel multiboot loader.
# One-time host setup: sudo apt install -y grub-pc-bin grub-common xorriso mtools
iso: kernel.bin grub.cfg
	@command -v grub-mkrescue >/dev/null 2>&1 || { \
	    echo "grub-mkrescue not found. Install it once with:"; \
	    echo "  sudo apt install -y grub-pc-bin grub-common xorriso mtools"; \
	    exit 1; }
	rm -rf isodir
	mkdir -p isodir/boot/grub
	cp kernel.bin isodir/boot/kernel.bin
	cp grub.cfg isodir/boot/grub/grub.cfg
	grub-mkrescue -o miniOS.iso isodir
	@echo "Built miniOS.iso -- boot with 'make run-iso' or attach it to a VM."

# Boot the ISO through GRUB (BIOS), exactly as a VM/real machine would.
run-iso: iso
	$(QEMU) -cdrom miniOS.iso

run-headless: kernel.bin $(ATA_IMAGE)
	timeout 5s $(QEMU) $(ATA_DRIVE) -display none -serial none -debugcon stdio \
	    -no-reboot -no-shutdown -kernel kernel.bin || test $$? -eq 124

test: unit test-ata-absent test-boot test-iso test-stress test-shell

# Boot the real GRUB/ISO path, the one used on VMs and hardware. The other
# targets all use QEMU's -kernel multiboot loader, which bypasses the
# bootloader entirely -- so without this, nothing verified that the ISO the
# README tells people to build actually boots. Asserting the Multiboot memory
# map in particular proves GRUB handed us a valid info structure, which is the
# substantive difference between the two paths. The ATA disk is attached as
# well so the disk-backed filesystems are exercised under this boot too.
# Skipped (not failed) when the ISO tooling is absent, since it is an optional
# host dependency that `make test` should not hard-require.
test-iso: kernel.bin grub.cfg $(ATA_IMAGE)
	@if ! command -v grub-mkrescue >/dev/null 2>&1; then \
	    echo "SKIP test-iso: grub-mkrescue not installed"; \
	    echo "  (sudo apt install -y grub-pc-bin grub-common xorriso mtools)"; \
	    exit 0; \
	fi; \
	$(MAKE) iso >/dev/null || exit 1; \
	log=$$(mktemp); \
	status=0; \
	timeout 8s $(QEMU) -cdrom miniOS.iso $(ATA_DRIVE) -display none \
	    -serial none -debugcon stdio -no-reboot -no-shutdown \
	    > $$log 2>&1 || status=$$?; \
	cat $$log; \
	grep -q "Initialized PMM from Multiboot memory map." $$log && \
	grep -q "Mounted DiskFS at /disk." $$log && \
	grep -q "Mounted FAT16 at /fat." $$log && \
	grep -q "Mounted procfs at /proc." $$log && \
	grep -q "miniOS shell" $$log; result=$$?; \
	rm -f $$log; \
	test $$result -eq 0 && test $$status -eq 124

test-ata-absent: kernel.bin
	@log=$$(mktemp); \
	status=0; \
	timeout 5s $(QEMU) -display none -serial none -debugcon stdio \
	    -no-reboot -no-shutdown -kernel kernel.bin > $$log 2>&1 || status=$$?; \
	cat $$log; \
	grep -q "ATA primary master not detected." $$log; result=$$?; \
	rm -f $$log; \
	test $$result -eq 0 && test $$status -eq 124

test-boot: kernel.bin $(ATA_IMAGE)
	@log=$$(mktemp); \
	status=0; \
	timeout 5s $(QEMU) $(ATA_DRIVE) -display none -serial none -debugcon stdio \
	    -no-reboot -no-shutdown -kernel kernel.bin > $$log 2>&1 || status=$$?; \
	cat $$log; \
	grep -q "miniOS shell" $$log; result=$$?; \
	rm -f $$log; \
	test $$result -eq 0 && test $$status -eq 124

# Drive the shell through QEMU's monitor socket instead of fixed sleeps.  The
# ring-3 stress binary covers integrated failure modes (faults, preemption,
# exhaustion and teardown), then the harness runs it a second time and requires
# the post-run resource snapshots to be byte-for-byte stable.
test-stress: kernel.bin $(ATA_IMAGE) tests/run_qemu_stress.py
	python3 tests/run_qemu_stress.py --qemu "$(QEMU)" \
	    --kernel kernel.bin --disk $(ATA_IMAGE) --log tests/qemu-stress.log

# Optional mutation proof for the new QEMU suite.  Each mutation is restored in
# an EXIT trap and must fail through its named stress assertion, not a timeout.
test-stress-mutants: tests/run_qemu_stress_mutants.sh tests/apply_mutation.py
	bash tests/run_qemu_stress_mutants.sh

test-shell: kernel.bin $(ATA_IMAGE)
	@log=$$(mktemp); \
	send_keys() { \
	    for key in "$$@"; do printf 'sendkey %s\n' "$$key"; sleep 0.08; done; \
	}; \
	{ \
	    sleep 1; \
	    send_keys h e l p ret; sleep 0.5; \
	    send_keys l s ret; sleep 0.5; \
	    send_keys e c h o spc w o r l d ret; sleep 0.5; \
	    send_keys p s ret; sleep 0.5; \
	    send_keys e c h o spc b g m a r k spc shift-7 ret; sleep 0.5; \
	    send_keys k i l l spc 9 9 9 ret; sleep 0.5; \
	    send_keys h e l l o ret; sleep 1; \
	    send_keys c a t ret; sleep 1; \
	    send_keys f s t e s t ret; sleep 1; \
	    send_keys m a l l o c t e s t ret; sleep 2; \
	    send_keys e c h o spc r e d i r e c t e d spc shift-dot spc o u t dot t x t ret; sleep 0.6; \
	    send_keys c a t spc o u t dot t x t ret; sleep 0.6; \
	    send_keys w c spc shift-comma spc o u t dot t x t ret; sleep 0.6; \
	    send_keys e c h o spc m o r e spc shift-dot shift-dot spc o u t dot t x t ret; sleep 0.6; \
	    send_keys w c spc shift-comma spc o u t dot t x t ret; sleep 0.6; \
	    send_keys e c h o spc h e l l o spc w o r l d spc shift-backslash spc w c ret; sleep 1; \
	    send_keys c a t spc r e a d m e dot t x t spc shift-backslash spc g r e p spc r i n g spc shift-backslash spc w c ret; sleep 1.5; \
	    send_keys c a t spc r e a d m e dot t x t spc shift-backslash spc g r e p spc z z z z spc shift-backslash spc w c ret; sleep 1.5; \
	    send_keys e c h o spc c h a r l i e spc shift-dot spc n a m e s dot t x t ret; sleep 0.5; \
	    send_keys e c h o spc a l p h a spc shift-dot shift-dot spc n a m e s dot t x t ret; sleep 0.5; \
	    send_keys e c h o spc b r a v o spc shift-dot shift-dot spc n a m e s dot t x t ret; sleep 0.5; \
	    send_keys s o r t spc shift-comma spc n a m e s dot t x t spc shift-backslash spc h e a d spc 1 ret; sleep 1.3; \
	    send_keys s o r t spc shift-comma spc n a m e s dot t x t spc shift-backslash spc t a i l spc 1 ret; sleep 1.3; \
	    send_keys r m spc n a m e s dot t x t ret; sleep 0.4; \
	    send_keys c d spc d i s k ret; sleep 0.4; \
	    send_keys p w d ret; sleep 0.4; \
	    send_keys e c h o spc i n d i s k spc shift-dot spc l o c a l dot t x t ret; sleep 0.5; \
	    send_keys c a t spc l o c a l dot t x t ret; sleep 0.6; \
	    send_keys r m spc l o c a l dot t x t ret; sleep 0.4; \
	    send_keys c d spc dot dot ret; sleep 0.4; \
	    send_keys p w d ret; sleep 0.4; \
	    send_keys m k d i r spc s u b ret; sleep 0.4; \
	    send_keys c d spc s u b ret; sleep 0.3; \
	    send_keys p w d ret; sleep 0.3; \
	    send_keys c d spc dot dot ret; sleep 0.3; \
	    send_keys t o u c h spc t f dot t x t ret; sleep 0.4; \
	    send_keys e c h o spc c o p y d a t a spc shift-dot spc a dot t x t ret; sleep 0.5; \
	    send_keys c p spc a dot t x t spc b dot t x t ret; sleep 0.4; \
	    send_keys m v spc b dot t x t spc c dot t x t ret; sleep 0.4; \
	    send_keys c a t spc c dot t x t ret; sleep 0.5; \
	    send_keys r m spc a dot t x t spc c dot t x t spc t f dot t x t ret; sleep 0.5; \
	    send_keys r m d i r spc s u b ret; sleep 0.4; \
	    send_keys l s spc f a t ret; sleep 0.5; \
	    send_keys c a t spc f a t slash h e l l o dot t x t ret; sleep 0.6; \
	    send_keys c a t spc f a t slash d o c s slash n o t e dot t x t ret; sleep 0.6; \
	    send_keys e c h o spc f a t w r i t e spc shift-dot spc f a t slash n e w dot t x t ret; sleep 0.6; \
	    send_keys c a t spc f a t slash n e w dot t x t ret; sleep 0.6; \
	    send_keys r m spc f a t slash n e w dot t x t ret; sleep 0.4; \
	    send_keys s i g t e s t ret; sleep 1.2; \
	    send_keys ctrl-c; sleep 1.2; \
	    send_keys s i g i p c ret; sleep 2.5; \
	    send_keys f o r k t e s t ret; sleep 1.5; \
	    send_keys e x e c d e m o ret; sleep 1.5; \
	    send_keys d e m a n d t e s t ret; sleep 1.2; \
	    send_keys s i g c h l d ret; sleep 2; \
	    send_keys w a i t d e m o ret; sleep 2.5; \
	    send_keys c w d d e m o ret; sleep 1.2; \
	    send_keys s t a t d e m o ret; sleep 1.2; \
	    send_keys c o w s t r e s s ret; sleep 1.5; \
	    send_keys a l a r m d e m o ret; sleep 1.5; \
	    send_keys p a u s e d e m o ret; sleep 2; \
	    send_keys p i p e d e m o ret; sleep 1.5; \
	    send_keys j o b c t l ret; sleep 2.5; \
	    send_keys u p t i m e ret; sleep 0.5; \
	    send_keys d a t e ret; sleep 0.5; \
	    send_keys c p u t i m e ret; sleep 2.5; \
	    send_keys s h m t e s t ret; sleep 1.5; \
	    send_keys s e m t e s t ret; sleep 1.5; \
	    send_keys m m a p t e s t ret; sleep 3; \
	    send_keys t h r e a d t e s t ret; sleep 3; \
	    send_keys t h r e a d e x i t ret; sleep 1.5; \
	    send_keys e x e c g u a r d ret; sleep 1.5; \
	    send_keys r a m g r o w ret; sleep 1.5; \
	    send_keys p a t h l i m ret; sleep 1.5; \
	    send_keys r e d i r r e f ret; sleep 1.5; \
	    send_keys r m spc r r dot t m p ret; sleep 0.6; \
	    send_keys f a t r e f ret; sleep 1.5; \
	    send_keys c p spc h e l l o spc f a t slash h e l l o ret; sleep 2.5; \
	    send_keys f a t slash h e l l o ret; sleep 1.5; \
	    send_keys r m spc f a t slash h e l l o ret; sleep 0.6; \
	    send_keys f a t g r o w ret; sleep 2; \
	    send_keys f o r k r e d i r ret; sleep 2; \
	    send_keys s i g r e t g u a r d ret; sleep 1.5; \
	    send_keys s i g f l a g s ret; sleep 1.5; \
	    send_keys k i l l t h r e a d spc shift-7 ret; sleep 2; \
	    send_keys k i l l w a i t spc shift-7 ret; sleep 3; \
	    send_keys b i g s e e k ret; sleep 2; \
	    send_keys l s spc slash p r o c ret; sleep 0.8; \
	    send_keys c a t spc slash p r o c slash p r o c e s s e s ret; sleep 0.8; \
	    send_keys c a t spc slash p r o c slash s e l f slash s t a t u s ret; sleep 0.8; \
	    send_keys u s h ret; sleep 0.8; \
	    send_keys e c h o spc u s h w o r k s ret; sleep 0.8; \
	    send_keys d a t e ret; sleep 0.8; \
	    send_keys u p t i m e ret; sleep 1.5; \
	    send_keys c d spc f a t ret; sleep 0.5; \
	    send_keys c a t spc h e l l o dot t x t ret; sleep 0.8; \
	    send_keys c d spc slash ret; sleep 0.5; \
	    send_keys e c h o spc r e d i r o k spc shift-dot spc r f dot t x t ret; sleep 0.8; \
	    send_keys c a t spc r f dot t x t ret; sleep 0.8; \
	    send_keys w c spc shift-comma spc r f dot t x t ret; sleep 0.8; \
	    send_keys e c h o spc a spc b spc c spc shift-backslash spc w c ret; sleep 1.0; \
	    send_keys c a t spc r e a d m e dot t x t spc shift-backslash spc g r e p spc r i n g spc shift-backslash spc w c ret; sleep 1.5; \
	    send_keys e c h o spc u s h b g spc shift-7 ret; sleep 1.0; \
	    send_keys p w d ret; sleep 0.6; \
	    send_keys m k d i r spc u s h d i r ret; sleep 0.5; \
	    send_keys t o u c h spc u s h f ret; sleep 0.5; \
	    send_keys l s ret; sleep 0.8; \
	    send_keys r m spc u s h f ret; sleep 0.5; \
	    send_keys r m d i r spc u s h d i r ret; sleep 0.5; \
	    send_keys e x p o r t spc g r e e t equal e n v w o r k s ret; sleep 0.5; \
	    send_keys e c h o spc shift-4 g r e e t ret; sleep 0.8; \
	    send_keys p r i n t e n v spc g r e e t ret; sleep 0.8; \
	    send_keys p s ret; sleep 0.8; \
	    send_keys k i l l spc 9 9 9 ret; sleep 0.6; \
	    send_keys e x i t ret; sleep 0.6; \
	    send_keys f a u l t ret; sleep 1; \
	    send_keys b a d p t r ret; sleep 1; \
	    send_keys s p a w n e r ret; sleep 2; \
	    send_keys o r p h a n ret; sleep 2; \
	    send_keys h e l l o ret; sleep 1; \
	    send_keys c a t ret; sleep 1; \
	    send_keys m e m ret; sleep 0.5; \
	    send_keys h e a p t e s t ret; sleep 0.5; \
	    send_keys h e a p t e s t ret; sleep 0.5; \
	    send_keys p m t e s t ret; sleep 0.5; \
	    send_keys a t a t e s t ret; sleep 0.5; \
	    send_keys d i s k t e s t ret; sleep 0.5; \
	    send_keys m e m ret; sleep 0.5; \
	    send_keys c l e a r ret; sleep 0.5; \
	    echo quit; \
	} | timeout 290s $(QEMU) -rtc base=2020-01-01T00:00:00 $(ATA_DRIVE) -display none -monitor stdio -serial none \
	    -debugcon file:$$log -no-reboot -no-shutdown -kernel kernel.bin \
	    >/dev/null 2>&1; \
	status=$$?; \
	cat $$log; \
	grep -q "Built-in commands:" $$log && \
	grep -q "^hello$$" $$log && \
	grep -q "^cat$$" $$log && \
	grep -q "^fault$$" $$log && \
	grep -q "^badptr$$" $$log && \
	grep -q "^worker$$" $$log && \
	grep -q "^spawner$$" $$log && \
	grep -q "^orphan$$" $$log && \
	grep -q "^sleeptest$$" $$log && \
	grep -q "^fstest$$" $$log && \
	grep -q "^echo$$" $$log && \
	grep -q "^malloctest$$" $$log && \
	grep -q "^wc$$" $$log && \
	grep -q "^grep$$" $$log && \
	grep -q "^head$$" $$log && \
	grep -q "^tail$$" $$log && \
	grep -q "^sort$$" $$log && \
	grep -q "^sigtest$$" $$log && \
	grep -q "^sigipc$$" $$log && \
	grep -q "^forktest$$" $$log && \
	grep -q "^execdemo$$" $$log && \
	grep -q "^demandtest$$" $$log && \
	grep -q "^sigchld$$" $$log && \
	grep -q "^waitdemo$$" $$log && \
	grep -q "^cwddemo$$" $$log && \
	grep -q "^statdemo$$" $$log && \
	grep -q "^cowstress$$" $$log && \
	grep -q "^alarmdemo$$" $$log && \
	grep -q "^pausedemo$$" $$log && \
	grep -q "^pipedemo$$" $$log && \
	grep -q "^jobctl$$" $$log && \
	grep -q "^uptime$$" $$log && \
	grep -q "^date$$" $$log && \
	grep -q "^printenv$$" $$log && \
	grep -q "^cputime$$" $$log && \
	grep -q "^shmtest$$" $$log && \
	grep -q "^semtest$$" $$log && \
	grep -q "^mmaptest$$" $$log && \
	grep -q "^threadtest$$" $$log && \
	grep -q "^threadexit$$" $$log && \
	grep -q "^execguard$$" $$log && \
	grep -q "^ramgrow$$" $$log && \
	grep -q "^pathlim$$" $$log && \
	grep -q "^redirref$$" $$log && \
	grep -q "^fatref$$" $$log && \
	grep -q "^forkredir$$" $$log && \
	grep -q "^sigretguard$$" $$log && \
	grep -q "^sigflags$$" $$log && \
	grep -q "^killthread$$" $$log && \
	grep -q "^killwait$$" $$log && \
	grep -q "^bigseek$$" $$log && \
	grep -q "^fatgrow$$" $$log && \
	grep -q "^stress$$" $$log && \
	grep -q "^ush$$" $$log && \
	grep -q "^readme.txt$$" $$log && \
	grep -q "^disk$$" $$log && \
	grep -q "^world$$" $$log && \
	grep -q "PID  PPID  STATE" $$log && \
	grep -q "processes)" $$log && \
	grep -q "bgmark" $$log && \
	grep -q "\[pid " $$log && \
	grep -q "kill: no such process" $$log && \
	grep -q "Mounted DiskFS at /disk." $$log && \
	grep -q "Mounted FAT16 at /fat." $$log && \
	grep -q "Mounted procfs at /proc." $$log && \
	grep -q "^count$$" $$log && \
	grep -q "^processes$$" $$log && \
	grep -q "^self$$" $$log && \
	grep -q "R cat" $$log && \
	grep -q "name=cat" $$log && \
	grep -q "state=R cpu=" $$log && \
	grep -q "\[cputime ok\]" $$log && \
	! grep -q "\[cputime FAIL\]" $$log && \
	grep -q "\[shm child] saw=100" $$log && \
	grep -q "\[shm parent] final=123" $$log && \
	grep -q "\[semtest] sum=15" $$log && \
	grep -q "\[semtest ok\]" $$log && \
	! grep -q "\[semtest FAIL\]" $$log && \
	grep -q "\[mmap] base=33554432" $$log && \
	grep -q "\[mmap ok\]" $$log && \
	! grep -q "\[mmap FAIL\]" $$log && \
	grep -q "\[munmap reuse ok\]" $$log && \
	grep -q "\[munmap hole ok\]" $$log && \
	grep -q "\[munmap fault ok\]" $$log && \
	! grep -q "\[munmap.*FAIL\]" $$log && \
	grep -q "\[thread] counter=400" $$log && \
	grep -q "\[thread ok\]" $$log && \
	! grep -q "\[thread FAIL\]" $$log && \
	grep -q "\[threadexit main done\]" $$log && \
	grep -q "\[threadexit worker done\]" $$log && \
	grep -q "\[execguard exec rejected\]" $$log && \
	grep -q "\[execguard worker ran\]" $$log && \
	grep -q "\[execguard done\]" $$log && \
	! grep -q "execguard exec WRONGLY" $$log && \
	grep -q "\[ramgrow bytes=2048\]" $$log && \
	grep -q "\[ramgrow ok\]" $$log && \
	! grep -q "\[ramgrow FAIL\]" $$log && \
	grep -q "\[pathlim overlong rejected\]" $$log && \
	grep -q "\[pathlim normal path ok\]" $$log && \
	grep -q "\[pathlim done\]" $$log && \
	! grep -q "pathlim overlong WRONGLY" $$log && \
	! grep -q "\[pathlim normal path FAIL\]" $$log && \
	grep -q "\[redirref inuse unlink refused\]" $$log && \
	grep -q "\[redirref stdin reads file\]" $$log && \
	grep -q "\[redirref done\]" $$log && \
	! grep -q "redirref inuse unlink WRONGLY" $$log && \
	! grep -q "\[redirref stdin read FAIL\]" $$log && \
	! grep -q "rm: cannot remove: rr.tmp" $$log && \
	grep -q "\[fatref inuse unlink refused\]" $$log && \
	grep -q "\[fatref content ok\]" $$log && \
	grep -q "\[fatref file intact\]" $$log && \
	grep -q "\[fatref done\]" $$log && \
	grep -q "\[fatgrow armed\]" $$log && \
	grep -q "\[fatgrow write refused\]" $$log && \
	grep -q "\[fatgrow size intact\]" $$log && \
	grep -q "\[fatgrow read intact\]" $$log && \
	grep -q "\[fatgrow done\]" $$log && \
	! grep -q "\[fatgrow write WRONGLY stored\]" $$log && \
	! grep -q "\[fatgrow size FAIL\]" $$log && \
	! grep -q "\[fatgrow read FAIL\]" $$log && \
	! grep -q "\[fatgrow\] " $$log && \
	! grep -q "fatref inuse unlink WRONGLY" $$log && \
	! grep -q "\[fatref content FAIL\]" $$log && \
	! grep -q "\[fatref file intact FAIL\]" $$log && \
	grep -q "\[forkredir inherited\]" $$log && \
	grep -q "\[forkredir done\]" $$log && \
	! grep -q "\[forkredir NOT inherited\]" $$log && \
	! grep -q "^childout$$" $$log && \
	grep -q "\[sigretguard arming\]" $$log && \
	grep -q "\[sigflags arming\]" $$log && \
	grep -q "\[sigflags iopl clear\]" $$log && \
	grep -q "\[sigflags nt clear\]" $$log && \
	grep -q "\[sigflags if set\]" $$log && \
	grep -q "\[sigflags done\]" $$log && \
	! grep -q "\[sigflags ESCALATED" $$log && \
	! grep -q "\[sigretguard SURVIVED\]" $$log && \
	grep -q "\[killthread armed\]" $$log && \
	! grep -q "\[killthread SURVIVED\]" $$log && \
	grep -q "\[killwait parked\]" $$log && \
	grep -q "\[killwait killer done\]" $$log && \
	! grep -q "\[killwait worker SURVIVED\]" $$log && \
	! grep -q "\[killwait parent SURVIVED\]" $$log && \
	! grep -q "\[killwait\] " $$log && \
	grep -q "\[bigseek arming\]" $$log && \
	grep -q "\[bigseek refused\]" $$log && \
	grep -q "\[bigseek survived\]" $$log && \
	! grep -q "\[bigseek unexpectedly wrote\]" $$log && \
	! grep -q "\[bigseek\] " $$log && \
	grep -q "^hello.txt$$" $$log && \
	grep -q "Hello from FAT16!" $$log && \
	grep -q "nested fat note" $$log && \
	grep -q "^fatwrite$$" $$log && \
	test $$(grep -c "Hello from user space!" $$log) -eq 3 && \
	test $$(grep -c "miniOS RAMFS file access works." $$log) -eq 4 && \
	grep -q "\[ramfs readdir test passed\]" $$log && \
	grep -q "\[ramfs path test passed\]" $$log && \
	grep -q "\[ramfs seek test passed\]" $$log && \
	grep -q "\[ramfs write test passed\]" $$log && \
	grep -q "\[diskfs vfs test passed\]" $$log && \
	grep -q "\[diskfs nested directory test passed\]" $$log && \
	grep -q "\[malloc test passed\]" $$log && \
	grep -q "^1 1 11$$" $$log && \
	grep -q "^2 2 16$$" $$log && \
	grep -q "^1 2 12$$" $$log && \
	grep -q "^1 12 62$$" $$log && \
	grep -q "^0 0 0$$" $$log && \
	grep -q "^alpha$$" $$log && \
	grep -q "^charlie$$" $$log && \
	grep -q "^/disk$$" $$log && \
	grep -q "^indisk$$" $$log && \
	grep -q "^/$$" $$log && \
	grep -q "^/sub$$" $$log && \
	grep -q "^copydata$$" $$log && \
	grep -q "\[caught SIGINT\]" $$log && \
	grep -q "\[sigtest exiting\]" $$log && \
	grep -q "\[child caught SIGUSR1\]" $$log && \
	grep -q "\[parent done\]" $$log && \
	grep -q "\[child\] shared=200" $$log && \
	grep -q "\[parent\] shared=100" $$log && \
	grep -q "\[fork test done\]" $$log && \
	grep -q "^execok$$" $$log && \
	grep -q "\[parent reaped exec child\]" $$log && \
	grep -q "\[demand paging ok\]" $$log && \
	grep -q "\[parent got SIGCHLD\]" $$log && \
	grep -q "\[child reaped\]" $$log && \
	grep -q "\[reaped status=7\]" $$log && \
	grep -q "\[reaped status=9\]" $$log && \
	grep -q "\[no more children\]" $$log && \
	grep -q "\[cwd2=/fat\]" $$log && \
	grep -q "\[relopen ok\]" $$log && \
	grep -q "\[chdir rejects missing dir\]" $$log && \
	grep -q "\[file size=18 type=1\]" $$log && \
	grep -q "\[fat is a directory\]" $$log && \
	grep -q "\[fstat size=18\]" $$log && \
	grep -q "\[child cow ok\]" $$log && \
	grep -q "\[parent cow ok\]" $$log && \
	grep -q "\[alarm fired\]" $$log && \
	grep -q "\[parent woken by child\]" $$log && \
	grep -q "^piped via dup2$$" $$log && \
	grep -q "\[pipe demo done\]" $$log && \
	grep -q "\[jc child] start" $$log && \
	grep -q "\[jc child] finished" $$log && \
	grep -q "\[jc parent] child reaped" $$log && \
	jc_win=$$(grep -n "window elapsed" $$log | head -1 | cut -d: -f1) && \
	jc_fin=$$(grep -n "\[jc child] finished" $$log | head -1 | cut -d: -f1) && \
	test -n "$$jc_win" && test -n "$$jc_fin" && test "$$jc_fin" -gt "$$jc_win" && \
	grep -q "s (ticks=" $$log && \
	grep -q "\[uptime ok\]" $$log && \
	! grep -q "\[uptime FAIL\]" $$log && \
	test $$(grep -c "2020-01-01 " $$log) -ge 2 && \
	grep -q "ush ready" $$log && \
	grep -q "^ushworks$$" $$log && \
	grep -q "^1 3 6$$" $$log && \
	grep -q "^redirok$$" $$log && \
	grep -q "^1 1 8$$" $$log && \
	grep -q "ushbg" $$log && \
	grep -q "\[bg done\]" $$log && \
	grep -q "^ushdir$$" $$log && \
	grep -q "^ushf$$" $$log && \
	grep -q "^envworks$$" $$log && \
	grep -q "^greet=envworks$$" $$log && \
	grep -q "ush procs:" $$log && \
	grep -q "ush: kill: no such process" $$log && \
	grep -q "ush bye" $$log && \
	grep -q "USER PAGE FAULT at address" $$log && \
	grep -q "Terminating user program." $$log && \
	grep -q "\[bad user pointer rejected\]" $$log && \
	grep -q "\[spawned two workers\]" $$log && \
	test $$(grep -c "\[worker done\]" $$log) -eq 3 && \
	grep -q "\[spawn/wait test passed\]" $$log && \
	grep -q "\[spawn_argv test passed\]" $$log && \
	grep -q "hello from spawn" $$log && \
	grep -q "\[per-process file table test passed\]" $$log && \
	test $$(grep -c "\[sleep test passed\]" $$log) -eq 2 && \
	grep -q "\[sleep queue test passed\]" $$log && \
	grep -q "\[orphan child launched\]" $$log && \
	! grep -q "test failed" $$log && \
	grep -q "PMM blocks: total=" $$log && \
	grep -q "User pages: accessible=0 spaces=0" $$log && \
	grep -q "Processes: running=0 zombies=0 peak=4" $$log && \
	grep -q "Tasks: blocked=0" $$log && \
	grep -q "Timers: sleeping=0" $$log && \
	grep -q "RAMFS nodes=60" $$log && \
	test $$(grep -c "\[heap test passed\]" $$log) -eq 2 && \
	grep -q "\[pmm high-memory test passed\]" $$log && \
	grep -q "\[ata pio read/write test passed\]" $$log && \
	grep -q "\[diskfs persistence test passed\]" $$log && \
	grep -q "ATA sectors: available=2048 reads=19 writes=76" $$log && \
	grep -q "DiskFS: mounted=1 generation=9 files=0" $$log && \
	grep -q "\[program exited\]" $$log && \
	! grep -q "exec: not found" $$log; \
	result=$$?; \
	rm -f $$log; \
	test $$result -eq 0 && test $$status -eq 0
