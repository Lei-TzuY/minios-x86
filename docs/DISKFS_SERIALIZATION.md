# DiskFS operation serialization

Inspected base: `e63d4218ea91069506b05944ead5a9198bf8568a` (main).
Open PRs #36 (interrupt entry), #37 (timer slots), #38 (thread join), and
#39 (header dependencies) are independent work and are not included here.
Main and all four PRs had successful CI when inspected. The recent storage
changes were ATA timeout recovery (`fae2038`) and post-write status checking
(`446c222`); neither added filesystem serialization.

## Entry paths and the original gap

- `sys_open`/`sys_create` use `resolve_fs`/`create_fs`, then `open_user_file`
  and `open_fs`. Read/write syscalls dispatch through `read_fs`/`write_fs`;
  namespace syscalls resolve parents and dispatch VFS callbacks.
- The syscall IDT entry in `isr.c` is an interrupt gate (`0xEE`). It clears IF,
  so current nonblocking DiskFS syscalls cannot preempt one another on this
  single CPU. Missing locks alone do **not** prove a syscall race today.
- `kernel.c` enables interrupts before entering `kernel_shell`. Shell copies,
  filesystem diagnostics, and ELF reads can therefore be preempted between
  sector requests. `timer_callback` calls `schedule` on each tick.
- DiskFS writes use read/modify/write of entire sectors. Two disjoint writes
  to one sector can each read the old sector, then the later write destroys
  the first update. A multi-sector read can mix file versions. A writer's
  directory-sector snapshot can overwrite another create; direct named I/O
  can address a slot that another operation has removed and reused.
- `ata_read_sector`/`ata_write_sector` serialize the command registers with
  IRQ exclusion, poll BSY/DRQ, transfer 256 words, and restore incoming IF.
  Writes check completion and flush status. This protects **one request**,
  not a read/modify/write sequence or directory/superblock publication.
  There is no IRQ completion queue today (`NIEN` disables device interrupts).
- Making ATA sleep would allow the same overlap even with incoming IF=0:
  `task_block_current` switches stacks and runs another task. Merely putting
  `cli` around DiskFS would not protect that suspended operation.

## Ownership and locking

| State | Protection |
| --- | --- |
| Superblock/generation/checksum, entries/parent links/lengths, mounted state | One volume operation gate |
| All sectors of a read/write, directory persistence and node publication | Same gate, held to completion or failure |
| Node callback/identity publication | Short IRQ critical section inside the gate; VFS dispatch loads callbacks before entering DiskFS |
| Open references, including in-flight VFS read/write pins | Short IRQ critical sections; never wait |
| Borrowed node and shared dirent results | Caller consumes/copies or acquires a reference before a scheduling point |
| ATA command registers and completion state | Driver-owned per-request IRQ exclusion; independent of the volume gate |

Every public DiskFS entry and VFS operation except reference callbacks acquires
the private gate once. Wrapper/helper separation provides one release path for
all returns; helpers never call the public wrappers. In particular, installation
calls the internal mount helper. Installation is boot initialization before
clients, not a runtime reset API.

Gate check, wait enrollment, acquire, release and wake are IRQ-protected. The
wait loop rechecks after every wake. IRQ flags are restored before the operation
body: a kernel caller remains preemptible, while a syscall keeps its incoming
IF=0. Ownership persists across device sleeps. The lock order is volume gate,
then driver request protection. The driver must not call back into DiskFS.
No IRQ handler may enter DiskFS. No VFS-wide or other-filesystem lock is added.

Waits use `task_block_current`, not `task_block_killable`: an ELF caller may
already own an address space/reference that it must release. A signal or kill
wake cannot discard that stack. Cancellation is deferred until the operation
returns to its caller; device waits must have bounded completion/timeout and
must return errors, preserve incoming IF, and never call `task_exit` while an
operation is owned. This matches the current nonpreemptible syscall execution
model without scheduler changes.

`open`/`close` must not take the sleepable gate. `task_exit` changes
`current_task` and activates the next address space **before** its exit callback
closes the retired task's files. Blocking there would operate on the wrong
scheduler context. Reference callbacks only adjust counts atomically. A VFS
read/write takes an extra reference before waiting, so a sibling closing the
last descriptor cannot let another operation reuse its node while queued or
in flight. It drops that pin before releasing the operation gate.

## Errors and boundaries

Metadata write failure already took the volume offline. Data-write failures
(including a read failure during read/modify/write) now do so too; earlier
sectors may have changed. Cached VFS read/write callbacks check mounted state,
so retained descriptors cannot continue I/O against partial cached metadata.
They can still close, and recovery can mount/format after references drain.
Ordinary read failures and preflight rejection keep their existing semantics.

This is operation serialization, not a journal or an atomic on-disk commit.
An ATA error or power loss can leave data/directory sectors partially written;
remount validates metadata but cannot restore the old file contents. There is
no safe general rollback if the device itself is failing.

Before enabling blocking ATA, the driver still needs request ownership across
sleep, IRQ completion, timeout recovery, and a boot-time polling path. Raw shell
ATA diagnostics currently access sectors 1/2, outside DiskFS's region; they
still need driver-level serialization when requests become blocking.

The VFS still returns borrowed pointers; kernel clients must retain references
across later node use. This change does not make whole path traversals, `stat`
snapshots, descriptor offsets or caller user buffers into transactions. Shared
descriptor close/reuse and user-buffer lifetime across a blocking syscall need
their own syscall-layer contracts before a general blocking-I/O rollout. The
gate is single-core and does not promise strict fairness or SMP support.

## Executable evidence

`tests/test_diskfs_operations.c` runs real DiskFS and VFS against a sector store.
Pthread stacks model suspended tasks; a CPU mutex is released at every device
sleep/context switch. Both an IF-enabled PIO return and an IF-disabled future
device sleep are exercised. It checks both write orders, coherent multi-sector
reads, directory publication/persistence, unlink/reuse, mount/format exclusion,
spurious/kill wakes, nonblocking reference cleanup, delayed waiter resumption,
and failures at each data/directory/superblock write through both APIs.

The unchanged main implementation fails the partial-write byte comparison and
other named assertions. Isolated mutations removing ownership, wakeup, queued
read pinning, offline-read rejection, or replacing the wait with a killable one
are rejected by named data/lifetime/liveness assertions.

`user/stress.c` additionally runs two ring-3 workers with independent descriptors,
shared-sector patches, private sector-spanning files, yields and verified
cleanup. The QEMU harness requires its success marker twice in one boot. The
current PIO driver cannot exercise sleeping ATA contention in that workload;
the hosted suite deliberately supplies that boundary.
