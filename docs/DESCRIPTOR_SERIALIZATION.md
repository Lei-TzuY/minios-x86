# Descriptor and standard-stream operations across filesystem waits

This follow-up uses main `e63d4218ea91069506b05944ead5a9198bf8568a` and
PR #40 head `e74d8875032e6bc7d3ba68ce5fb500a3e8b53b36`, freshly fetched.
Main and the PR were green; PRs #36–#39 still cover separate interrupt,
timer, thread-join and build work. There were no new source review findings
or open non-PR issues.

## The remaining caller-side gap

The DiskFS volume gate owns filesystem state, not a syscall's descriptor.
`sys_read_file` and `sys_write_file` previously sampled `open_files[slot][fd]`'s
node and offset before calling VFS, then incremented the table offset after
the callback returned. All threads in a process share that table.

Two writes that both reach a busy volume gate can therefore pass the same
old offset to DiskFS. DiskFS correctly serializes those writes, but the second
still overwrites the first. Two reads can return the same bytes while advancing
the offset twice. `seek`, `dup`, `dup2` and fork can sample incomplete offsets.
A sibling can close and reuse the slot while an I/O is asleep, so completion
increments the replacement descriptor's offset. DiskFS's temporary node pin
keeps the old node alive; it does not protect the replaced table entry.

This is reachable with the existing PIO driver: the kernel shell runs with
IF enabled and can be preempted while owning the volume gate. Two IF=0
syscalls then queue behind that owner. Future sleeping ATA adds more such
boundaries. No syscall-to-syscall timer preemption is assumed.

## Ownership and operation order

Each indexed descriptor (fd 3–10) has its own private sleepable gate in
`syscall.c`. The slot lock covers lookup, file I/O, offset commit, seek, fstat,
close and duplication. An active file operation keeps the descriptor's node
reference until it has committed its offset and released the gate.

A waiting call resolves the descriptor **after acquiring** the slot. Concurrent
calls may acquire in either order; strict FIFO and Unix open-file-description
semantics are not promised. If close wins, a later acquisition sees a closed
slot, or the new binding if it has been reused. It never finishes an old I/O
by changing the replacement binding. A suspended-stack test pins this rule
for both a closed slot and a slot reused as a pipe.

`dup2` locks its two table slots in ascending index order and releases in
reverse order. It samples and replaces only after owning both. An empty
slot held by a waiting `dup2` is reserved: `open`, `create`, `pipe` and `dup`
allocation skip it. `dup` and fork retain the existing ABI: independent
references and independent offsets copied after the source operation commits.
Only descriptor payload is copied; a child's or duplicate's gate never
inherits its source's locked state. Fork copies one slot at a time before
the child runs, and does not promise a whole-table transaction.

The lock order is ascending descriptor slots, then the DiskFS volume gate,
then ATA request protection. Filesystems never call back into descriptor APIs.
An unrelated RAMFS descriptor can progress while another descriptor waits
inside DiskFS. There is no process-wide or VFS-wide I/O lock.

## Blocking, cleanup and errors

Entry is through the existing IF=0 syscall gate. Check/enroll/acquire/release
and wakeups have short IRQ critical sections. Waits recheck the condition
after normal, spurious and kill wakeups. They return through their callers
rather than terminating the task inside the gate: fork may already own a
child address space and `dup2` may hold another descriptor gate.

Pipe operations release the descriptor gate before calling `pipe_read` or
`pipe_write`. IF remains clear until the pipe acquires its own endpoint
reference. Closing/replacing a pipe descriptor can therefore still wake EOF
or broken-pipe waiters. Pipe's existing kill cleanup owns that reference;
no new resource is stranded on its nonreturning kill path.

`syscall_close_user_files` remains nonblocking. Its callers are final-process
cleanup after the last task has exited, and cleanup of a child that never ran.
No live descriptor operation exists then. This matters because `task_exit`
has already changed `current_task` before the retired task's exit callback.

Every normal/error return releases acquired gates. Zero-length writes retain
their no-backend-I/O behavior; failed writes do not advance the offset. User
buffers are revalidated after descriptor acquisition, because mappings can
change while waiting for that gate.

## Standard streams: runtime operation ownership

The next live inspection found main unchanged and PR #40 at
`84150abaf7d7624e0ac81b20350e4b7c4908c5de`, with all three workflows passing.
`sys_read`/`sys_write` still sampled `process_t`'s stdin/stdout node and offset
before a filesystem wait, and `dup2` could replace them during that wait.
Tests on that exact head reproduce overlapping stdout records, repeated stdin
bytes and completion advancing a replacement stream's offset. The existing
preemptible kernel/PIO boundary also reproduces the lost stdout record.

Two private gates per process slot now protect runtime fd 0/1 independently.
They use the indexed table's IRQ-protected check/enroll/wake mechanism, and
cover binding lookup, file I/O and offset commit. `dup2(fd, 0/1)` owns the
standard stream **before** its indexed source, then samples both bindings;
an unrelated source can still close/reopen while dup2 waits for the stream.
This extends the ascending descriptor order without a process-wide I/O lock.
Waiting readers/writers resolve the winning binding only after acquisition.

The existing fork hook `syscall_copy_user_files` now snapshots standard streams
as well as indexed descriptors. It owns only one source at a time, retaining
both file and pipe references and the committed offset before the child runs.
`process_fork` no longer separately copies unlocked stream fields. The child's
gate state is not inherited. Fork still supplies independent offsets and does
not promise an atomic snapshot of the whole descriptor table or process memory.

Pipe dispatch releases the stream gate before entering pipe.c, which takes its
own endpoint pin before any wait. Keyboard dispatch releases it too: device
waits may terminate the task and must not strand a stream gate. An operation
already handed to a pipe/keyboard finishes on that device even if dup2 redirects
the stream later. Future calls use the new binding. Terminal output remains
nonblocking. User buffers are revalidated after acquiring a stream gate.
File read/write now retains their virtual mappings across subsequent filesystem
waits; see [FILE_IO_BUFFERS.md](FILE_IO_BUFFERS.md).

Final exit and failed-child teardown in process.c still release references
without acquiring a gate. Every live operation has returned before the last
task exits, so no gate survives process-slot reuse. Replacing a stream and
fork failure use the existing reference-release paths. SYS_WRITE retains its
existing zero-byte return on backend failure; no syscall ABI or errno mapping
changes here.

Suspended-stack tests cover both stream directions, current PIO preemption,
replacement, fork offsets/references, pipe inheritance, source reuse while
dup2 waits, late binding to a pipe, independent streams/processes, spurious and
kill wakeups, actual unmapping during gate wait, write failure and default
device dispatch. Eight isolated mutants fail named data/progress assertions:
missing ownership/wakeup, unlocked replacement/fork, killable ownership wait,
holding the stream over pipe/keyboard waits, and locking dup2's source first.

The QEMU stress controller forks a child with DiskFS stdin/stdout. Two threads
consume twelve uniquely numbered input records and produce twelve output
records in arbitrary order. The parent verifies every output record once and
unlinks both files after child exit. The validator requires the new success
marker twice, in addition to all existing stress/resource checks.

## Executable coverage and remaining work

`tests/test_fd_operations.c` uses real syscall, VFS, DiskFS, RAMFS and pipe code.
Pthread stacks model suspended tasks on one CPU, with real mapped user buffers
at 32 MiB. It exercises both write orders, sequential reads, the current
preemptible kernel/PIO boundary, seek/stat/dup/fork, close/reuse, opposite dup2
directions, empty-slot reservation, late lookup, spurious/kill wakes, errors,
real unmapping during gate wait, pipe close/replacement and independent I/O.
It is registered in both the native and ASan/UBSan gates.

The unchanged prior PR head fails named byte/offset assertions. Isolated
mutants dropping ownership, wakeup or reservation, copying lock state, reversing
lock order, or using a killable wait also fail named assertions. Mutations run
on scratch copies, never on the working tree.

The ring-3 stress workers additionally share one descriptor and write twelve
uniquely identified records in arbitrary worker order. They verify every
record exactly once, final and duplicated offsets, and final-reference cleanup.
The QEMU validator requires the success marker twice with stable resources.

The guarantees cover indexed descriptors and runtime standard-stream
syscalls/fork. `process_redirect` and `process_pipe` remain initialization APIs
whose documented precondition is "before the child's first run". Inspection
found that kernel-shell spawn publishes a runnable task before these setup
calls, with IF enabled. Enforcing initialization before task publication is a
separate existing launch-order gap; this change does not make arbitrary
cross-process redirection safe. Those APIs must not be used as runtime setters.

File read/write buffers now have mapping ownership across VFS/device sleep,
as described in [FILE_IO_BUFFERS.md](FILE_IO_BUFFERS.md). Pipe/keyboard buffer
cancellation and borrowed path/stat pointers remain separate integration work.
This is not DMA frame pinning. ATA still polls and DiskFS still has no journal
or crash-atomic metadata transaction. This gate assumes the existing single CPU
and IF=0 syscall ABI; it is not an SMP synchronization primitive.
