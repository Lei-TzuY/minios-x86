# User mappings across filesystem I/O

Fresh inspection found main at `e63d4218ea91069506b05944ead5a9198bf8568a`
and PR #40 at `636be97eaafc14e92825c020ac4c6f1cd8a32181`, with all three
workflows passing. PRs #36–#39 remain separate interrupt, timer, thread-join
and build work. No new main commits or open non-PR issues were present.

## Source evidence

`sys_read_file`, `sys_write_file`, and redirected `sys_read`/`sys_write`
validate a buffer after acquiring descriptor ownership. They then pass that
user virtual address through VFS into DiskFS. `diskfs_read_slot` copies each
sector into the buffer after its ATA read; `diskfs_write_slot` reads source
bytes after an RMW read and between sector writes.

Validation alone cannot cover a subsequent volume/device wait. A sibling's
`sys_munmap` used to release the reservation bitmap through `process_ext_free`
and then tear down every PTE, including an in-flight operation's buffer.
The resumed I/O would no longer have the mapping it had validated.

This boundary already exists when a preemptible kernel caller owns DiskFS
and an IF=0 syscall waits for it. Future sleeping ATA adds waits inside the
sector loops. The tests model both; they do not assume timer preemption
between IF=0 syscalls. Prior head fails the new mapping-ownership assertions.

## Contract and scope

After descriptor acquisition and buffer revalidation, file I/O registers its
nonempty half-open virtual range on a private per-process list in syscall.c.
A single wrapper owns that record from entry into VFS until VFS returns,
including volume waiting, all sector transfers, and error returns. The record
lives on the suspended syscall stack and is removed by identity: overlapping
operations can complete in either order. Zero-length calls register nothing.
No heap allocation, data copy, process-wide I/O lock, or new wait is needed.

`sys_munmap` bounds its range before calculating the end, then checks all
active file ranges in the calling process. Any overlap returns -1 before
changing any reservation or PTE. The caller may retry after I/O completes.
An unrelated page, or the same virtual address in another process, remains
independent. Ranges spanning multiple pages and multiple owners of one page
retain all owners until their corresponding operations return.

A task waiting for a descriptor has not submitted file I/O yet. Its buffer
can still be unmapped; the existing post-acquisition validation then rejects
the call before entering VFS. The original revalidation tests now also use
the real munmap syscall and reservation allocator.

All list accesses run under the current single-CPU IF=0 syscall ABI. DiskFS
and future device waits must preserve IF and return through caller cleanup,
including kill wakeups. They must not terminate the task or enable timer
preemption while these resources are owned. The existing descriptor/volume
ordering is unchanged. There is no additional lock to nest or wake.

The last task cannot finish until its file I/O has returned. Current process
teardown keeps the address space alive until then; exec rejects concurrent
threads. Shrinking sbrk does not unmap pages. The only production caller of
process_ext_free/paging_unmap_user_page is sys_munmap. These facts make its
pre-teardown overlap check sufficient for this bounded contract; a future
mapping-removal API must participate too.

This preserves virtual mappings, not physical frames for DMA. Fork/COW can
still replace a backing frame while preserving the virtual address. The
caller also retains responsibility for concurrent modifications to buffer
bytes. Neither byte snapshots nor DMA support are promised.

## Executable verification

The registered descriptor-operation suite now includes real process.c mmap
reservation logic, alongside syscall/VFS/DiskFS/RAMFS/pipe implementations.
Suspended pthread stacks simulate one CPU. PTEs are modeled per address space;
host backing remains available during the new tests so lost ownership is
reported by named assertions rather than a later host fault.

Coverage includes indexed and redirected reads/writes, cross-page ranges,
current PIO volume waits, sleeping device boundaries, whole-range rejection
without partial teardown, adjacent-page and other-process progress, multiple
overlapping owners with both completion orders, read/RMW/write failures, EOF,
zero/invalid I/O, kill/spurious wakeups, retry and address reuse. The expanded
suite passes 3,462 checks under local 64-bit ASan/UBSan; VM lifecycle passes
36 checks. Standard i386 gates are verified in CI because this local runtime
cannot execute i386 hosted binaries.

Seven isolated mutants are rejected by named assertions: omitted overlap
check, retaining only the first page, releasing before I/O, dropping another
owner on completion, sharing ownership between processes, bypassing stdin
protection, and bypassing indexed writes. The prior PR head is rejected too.
Mutation copies are temporary; source is never mutated in place.

The ring-3 stress workload additionally exercises cross-page mmap buffers
through indexed and redirected file I/O, writes into fork/COW mappings,
parent/child isolation, munmap/reuse, and reference cleanup. Its new success
marker is required twice alongside every existing resource assertion.

## Remaining boundaries

Pipe/keyboard waits can terminate a task without unwinding the syscall stack.
They do not register these records; adding ownership there requires their
own cancellation cleanup contract. Borrowed stat/readdir output pointers and
exec argument lifetimes also remain separate work. This change covers file
read/write buffers, not every syscall pointer.

ATA still polls. Request completion/timeout recovery, boot-time fallback,
DMA frame ownership, and SMP/preemptible-syscall synchronization remain future
work. DiskFS serialization still does not provide crash-atomic metadata or
sector rollback. See DISKFS_SERIALIZATION.md and DESCRIPTOR_SERIALIZATION.md
for the wider operation and descriptor contracts.
