# miniOS

A small but feature-rich hobby operating system for 32-bit x86 (i386), written from scratch in C and assembly. It boots via Multiboot/GRUB, runs user programs in ring 3 through a system-call interface, and includes automated native and QEMU-based testing.

> **Status:** educational / hobby project. Built for systems learning, debugging, and experimentation — not production use.

## Why this project is interesting

miniOS is not just a bootable kernel demo. It implements and tests several interacting OS subsystems end to end:

- protected mode, GDT/IDT, PIC/IRQ, timer and keyboard input
- physical memory management, paging, copy-on-write `fork`, demand paging, `mmap`/`munmap`, shared memory
- preemptive round-robin scheduling, processes, threads, signals, job control, semaphores and pipes
- VFS with RAMFS, ATA-backed DiskFS, read/write FAT16 and a synthetic `/proc`
- ELF loading and ring-3 user programs
- a user shell with pipes, redirection, background jobs and shell variables

The project is developed with an audit-driven testing loop: bugs and invariants are turned into native tests, mutation checks, and end-to-end regressions instead of being left as informal assumptions.

## Verification at a glance

Recent project checkpoints include:

- **52 system calls** and **53 user programs / demos**
- native host-side unit suites for kernel logic
- QEMU end-to-end tests, including the real GRUB/ISO boot path
- mutation testing used to validate whether tests actually detect injected faults
- regression tests for issues involving ELF loading, integer overflow, user-pointer validation, process lifecycle, descriptor ownership, filesystems, ATA behavior and signal handling

The exact current syscall, user-program, native-suite and QEMU-target lists are machine-derived in [docs/PROJECT_INVENTORY.md](docs/PROJECT_INVENTORY.md). `make static-analysis` rejects both generated-inventory drift and stale headline counts in the main project docs.

Build and run the complete validation path with:

```sh
make clean
make all
make test
```

## Architecture

```text
+-----------------------------+
|        User programs        |
| shell / tools / demos       |
+-------------+---------------+
              | syscalls
+-------------v---------------+
| Process / signal / IPC      |
| scheduler / fork / exec     |
+-------------+---------------+
              |
+-------------v---------------+
| VFS + filesystems           |
| RAMFS / DiskFS / FAT16      |
+-------------+---------------+
              |
+-------------v---------------+
| Memory + CPU core           |
| PMM / paging / COW / IRQ    |
+-------------+---------------+
              |
+-------------v---------------+
| x86 hardware / QEMU         |
+-----------------------------+
```

## Features

### Boot & CPU
- Multiboot kernel
- 32-bit protected mode
- GDT / IDT
- PIC / IRQ handling

### Memory
- physical memory manager
- paging with **copy-on-write fork**
- **demand paging**
- `mmap` / `munmap` region with page-granular reuse
- kernel heap
- inter-process **shared memory**

### Processes & scheduling
- preemptive round-robin scheduler
- `fork` / `execv` / `wait` / `waitpid` / `exit`
- per-process CPU-time accounting
- threads and lifecycle handling

### Signals & IPC
- SIGINT / KILL / USR1 / ALRM / TERM / CHLD
- SIGSTOP / SIGCONT job control
- pipes
- shared memory
- counting semaphores

### Filesystems
A unified VFS over:

- RAMFS
- ATA-backed DiskFS
- read/write FAT16
- synthetic `/proc`

Programs can be executed from mounted filesystems rather than only from the built-in RAMFS.

### User land
- 52 system calls
- 53 user programs / demos
- kernel shell
- ring-3 user shell (`ush`)
- pipes, `dup`/`dup2`, redirection, background jobs and shell variables

## Testing strategy

miniOS uses two complementary layers.

**Native unit tests** compile pure-logic kernel modules for the host so failures surface quickly without waiting for emulation.

**QEMU end-to-end tests** exercise the integrated kernel, user programs, shell
behavior and boot path. The dedicated stress regression drives memory allocation
and paging, timer interrupts and preemption, context switching, syscalls, all
writable filesystems, process/thread teardown, invalid user pointers and exact
resource-exhaustion boundaries. The user allocator must reach a real NULL
result, preserve every live chunk, then coalesce enough freed space for a large
reuse allocation. It also repeatedly terminates a resource-bearing child through
real ring-3 #PF, #DE, #UD and #GP exceptions, proving both CPU exception isolation
and abnormal cleanup of files, pipes, heap and mmap pages. The suite runs twice
in one boot and requires the post-run resource snapshots to match exactly.

Mutation testing is used selectively to answer a harder question than line coverage: *would the suite actually fail if this logic were wrong?*

This has been especially useful around boundary conditions, overflow checks, ownership/refcount invariants, blocking behavior and hardware-driver state machines.

## Requirements

Linux toolchain, or WSL on Windows:

```sh
sudo apt install -y build-essential gcc-multilib qemu-system-x86 python3 cppcheck
```

For the bootable ISO target:

```sh
sudo apt install -y grub-pc-bin grub-common xorriso mtools
```

## Build and run

```sh
make              # build kernel.bin
make run          # run in QEMU with a test disk attached
make unit         # native unit tests only
make test         # native + QEMU / ISO validation
make test-stress  # focused ring-3 stress run, twice in one QEMU boot
make sanitize     # hosted suites under AddressSanitizer + UBSan
make static-analysis       # Python + inventory + shell syntax checks, then cppcheck
make test-stress-mutants   # prove all seven capacity/leak/exception gates fire
make iso          # produce miniOS.iso
make run-iso      # boot the ISO through GRUB in QEMU
```

On Windows, prefix commands with `wsl`.

## Try it

At the kernel prompt:

```sh
ls /proc
cat /proc/processes
uptime
date
ush
```

## Project layout

```text
kernel / drivers / MM / VFS   top-level C and assembly sources
user/                         ring-3 programs and syscall wrappers
tests/                        native unit tests
docs/PROJECT_INVENTORY.md     generated syscall/program/test inventory
gen_*.py                      image / ELF generation helpers
Makefile                      build, run, ISO and test targets
```

## Scope and limitations

This is intentionally an educational OS, not a Linux replacement. The project favors understandable implementations, executable invariants, and reproducible bug investigations over production compatibility or performance.

Known limitations and design tradeoffs are documented in the repository rather than hidden behind feature claims.

## License

MIT — see [LICENSE](LICENSE).

---

## 中文簡介

miniOS 是一個從零開始、以 C 與組合語言實作的 32 位元 x86 教學型作業系統。除了可開機核心之外，也包含 ring 3 使用者程式、行程與排程、COW fork、需求分頁、mmap、IPC、VFS、多種檔案系統、ATA、signals、shell，以及 native + QEMU 兩層測試。

這個專案特別重視「可驗證性」：除了功能實作，也會把實際找到的 bug、邊界條件與系統 invariant 轉成 regression test，並利用 mutation testing 檢查測試是否真的能抓到錯誤。
