# miniOS

A small but feature-rich hobby operating system for 32-bit x86 (i386), written
from scratch in C and assembly. It boots via Multiboot/GRUB, runs user programs
in ring 3 through a system-call interface, and ships with two interactive
shells and an automated test suite.

> **Status:** educational / hobby project. It is meant for learning and
> experimentation, not production use.

## Features

- **Boot & CPU:** Multiboot kernel, 32-bit protected mode, GDT/IDT, PIC/IRQ.
- **Memory:** physical memory manager, paging with **copy-on-write fork** and
  **demand paging**, an **mmap/munmap** region with page-granular address
  reuse, a kernel heap, and **shared memory** between processes.
- **Processes & scheduling:** preemptive round-robin scheduler;
  `fork` / `execv` / `wait` / `waitpid` / `exit`; per-process CPU-time accounting.
- **Signals & job control:** SIGINT/KILL/USR1/ALRM/TERM/CHLD, `pause`, `alarm`,
  `kill`, plus **SIGSTOP/SIGCONT job control**.
- **IPC:** pipes, signals, shared memory, and **counting semaphores**.
- **Filesystems:** a unified VFS over three backends — **RAMFS**, an ATA-backed
  **DiskFS**, and a read/write **FAT16** image — plus a synthetic **`/proc`**
  (`/proc/count`, `/proc/uptime`, `/proc/processes`, `/proc/self/status`).
- **Environment:** per-process environment variables inherited across
  `fork`/`execv`.
- **Time:** CMOS RTC wall clock (`date`), monotonic uptime, CPU accounting.
- **User land:** 51 system calls, 49 user programs/demos, a kernel-space shell
  and a **ring-3 user shell (`ush`)** with pipes, redirection, background jobs,
  and shell variables. Programs are loaded through the VFS, so an executable
  can be run from **any mounted filesystem** (e.g. `cp hello fat/hello` then
  `fat/hello`), not just the built-in RAMFS.
- **Quality:** two layers of testing. Native unit tests compile the pure-logic
  modules for the host and run in under a second; an end-to-end suite then
  drives the shell through the QEMU monitor, including a boot of the real
  GRUB/ISO path. `make test` runs all of it.

## Requirements

A Linux toolchain (native or **WSL** on Windows):

```sh
sudo apt install -y build-essential gcc-multilib qemu-system-x86 python3
# Only needed for the bootable ISO target:
sudo apt install -y grub-pc-bin grub-common xorriso mtools
```

## Build & run

```sh
make              # build kernel.bin
make run          # run in QEMU with a test disk attached
make unit         # native unit tests only (sub-second)
make test         # everything: unit tests, then the QEMU/ISO suite
```

`make unit` compiles the pure-logic kernel modules (`utils`, `fs` path
resolution, `pmm`, `heap`) for the host and exercises them directly, so a logic
regression surfaces immediately instead of after the multi-minute emulation
run. `make test` runs those first, then boots the kernel in QEMU — including
once through the real GRUB/ISO path, which is skipped with a notice if the ISO
tooling is not installed.

Build a GRUB-bootable ISO (for VirtualBox/VMware/real hardware):

```sh
make iso          # produces miniOS.iso
make run-iso      # boot the ISO through GRUB in QEMU
```

The resulting `miniOS.iso` can be attached as a boot CD to a VM, or written to
a USB stick. Use a **BIOS (legacy, non-UEFI)** VM with ~32 MB of RAM. To
exercise the disk-backed filesystems, attach a virtual hard disk as the primary
IDE/ATA device; without one, the kernel still boots straight to the shell and
RAMFS/FAT16/`/proc` remain available.

> On Windows, prefix commands with `wsl`, e.g. `wsl make test`.

## Trying it out

At the `>` prompt, type `help`. A few things to try:

```sh
ls /proc            # browse the synthetic process filesystem
cat /proc/processes # list running processes
uptime              # time since boot
date                # wall-clock time (CMOS RTC)
ush                 # drop into the ring-3 user shell
```

## Project layout

- Kernel C/ASM sources live at the top level (`kernel.c`, `paging.c`,
  `process.c`, `syscall.c`, filesystem and driver modules, `boot.s`, …).
- `user/` — ring-3 programs and the user shell, plus their syscall wrappers.
- `tests/` — native (host-compiled) unit tests for the pure-logic modules.
- `gen_*.py` — helpers that embed user ELFs and build the disk/FAT images.
- `Makefile` — build, `run`, `iso`, `unit`, and the `test*` targets.

## License

MIT — see [LICENSE](LICENSE).

---

## 中文簡介

miniOS 是一個從零開始、用 C 與組合語言寫的 32 位元 x86 業餘作業系統,透過
Multiboot/GRUB 開機,並以系統呼叫介面在 ring 3 執行使用者程式。

**主要功能**:保護模式 + GDT/IDT;分頁(寫時複製 fork、需求分頁)、行程間
**共享記憶體**;搶佔式排程與 `fork`/`execv`/`wait`/`exit`;完整信號模型與
**job control(SIGSTOP/SIGCONT)**;IPC(pipe、信號、共享記憶體、**計數號誌**);
三個檔案系統(RAMFS、ATA 的 DiskFS、可讀寫 FAT16)整合在 VFS 之下,外加合成的
**`/proc`**;per-process 環境變數;RTC 牆鐘時間與 CPU 時間計量;51 個系統呼叫、
49 支使用者程式、核心 shell 與 **ring-3 使用者 shell(`ush`)**;以及一套
兩層測試:原生單元測試(`make unit`,不到一秒)加上透過 QEMU monitor 驅動的
端對端測試(含真實 GRUB/ISO 開機路徑),`make test` 會全部跑過。

**這是教學/業餘性質的專案**,適合學習與實驗,非生產用途。

建置與執行見上方英文說明(`make` / `make run` / `make test` / `make iso`);
Windows 上請在指令前加 `wsl`。
