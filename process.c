#include "process.h"
#include "elf_loader.h"
#include "fs.h"
#include "irq.h"
#include "pipe.h"
#include "syscall.h"
#include "task.h"
#include "utils.h"

extern void enter_user_mode(uint32_t eip, uint32_t user_esp);
extern void enter_user_mode_iret(const fork_frame_t *frame);

static process_t processes[MAX_PROCESSES];
static int32_t next_pid = 1;
static uint32_t peak_process_count;

static uint32_t process_get_used_count(void) {
    uint32_t count = 0;

    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].state != PROCESS_UNUSED) count++;
    }
    return count;
}

/* Bounded copy into a fixed-size process name buffer (always null-terminated). */
static void process_copy_name(char *dest, const char *src) {
    int i = 0;

    if (src) {
        for (; i < PROCESS_NAME_MAX - 1 && src[i]; i++) dest[i] = src[i];
    }
    dest[i] = '\0';
}

static process_t *process_find(int32_t pid) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].state != PROCESS_UNUSED && processes[i].pid == pid) {
            return &processes[i];
        }
    }
    return NULL;
}

/* Return the next positive pid without overflowing int32_t or colliding with
 * a process that is still live after the counter wraps. Because allocation
 * only reaches this helper when a process-table slot is free, at most
 * MAX_PROCESSES - 1 candidates can be occupied. */
static int32_t process_allocate_pid(void) {
    for (int attempts = 0; attempts < MAX_PROCESSES; attempts++) {
        int32_t candidate = next_pid;

        next_pid = (next_pid == INT32_MAX) ? 1 : next_pid + 1;
        if (!process_find(candidate)) return candidate;
    }
    return -1;
}

static process_t *process_allocate(void) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].state == PROCESS_UNUSED) {
            process_t *process = &processes[i];
            int32_t pid = process_allocate_pid();

            if (pid < 1) return NULL;
            memset(process, 0, sizeof(*process));
            process->slot = i;
            process->pid = pid;
            process->parent_pid = process_get_current_pid();
            process->state = PROCESS_RUNNING;
            process->cwd[0] = '/';
            process->cwd[1] = '\0';
            /* ext_map is already zero (memset above): empty mmap region. */
            return process;
        }
    }
    return NULL;
}

static void process_release(process_t *process) {
    uint32_t slot;

    if (!process) return;
#ifdef HOSTED_TEST
    /* CAP20's hosted lifecycle model must distinguish a slot becoming UNUSED
     * from it being released twice. This hook is compiled out of the kernel,
     * so it cannot alter its ABI or code generation. */
    extern void process_test_observe_release(process_t *process);
    process_test_observe_release(process);
#endif
    slot = process->slot;
    memset(process, 0, sizeof(*process));
    process->slot = slot;
}

static void process_task_runner(void) {
    process_t *process = process_get_current();

    if (!process) task_exit(-1);
    enter_user_mode(process->entry_point, process->user_stack);
}

/* First thing a forked child runs: resume the parent's user context (eax = 0). */
static void fork_child_entry(void) {
    process_t *process = process_get_current();

    if (!process) task_exit(-1);
    enter_user_mode_iret(&process->fork_frame);
}

/* Full process-level teardown: reparent children, close pipeline endpoints and
 * open files, free the address space, and transition to PROCESS_ZOMBIE. Only
 * safe to run once every task sharing `process`'s address space (the main
 * task plus any SYS_THREAD_CREATE threads) has stopped running -- see the
 * thread_count deferral in process_task_exit()/thread_on_exit() below. */
static void process_finish_exit(process_t *process, int32_t status) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *child = &processes[i];

        if (child->state == PROCESS_UNUSED ||
            child->parent_pid != process->pid) {
            continue;
        }

        if (child->state == PROCESS_ZOMBIE) {
            process_release(child);
        } else {
            child->parent_pid = 0;
            child->auto_reap = 1;
        }
    }

    /* Close pipeline endpoints so the peer process sees EOF / broken pipe. */
    if (process->stdout_pipe) {
        pipe_close_write(process->stdout_pipe);
        process->stdout_pipe = NULL;
    }
    if (process->stdin_pipe) {
        pipe_close_read(process->stdin_pipe);
        process->stdin_pipe = NULL;
    }

    /* Drop the references taken when these nodes were installed as this
     * process's standard streams (process_redirect / sys_dup2). Without the
     * matching release the file could never be unlinked afterwards. */
    if (process->stdout_node) {
        close_fs(process->stdout_node);
        process->stdout_node = NULL;
    }
    if (process->stdin_node) {
        close_fs(process->stdin_node);
        process->stdin_node = NULL;
    }

    syscall_close_user_files(process);
    paging_destroy_user_address_space(process->address_space);
    process->address_space = NULL;
    process->task = NULL;
    process->exit_status = status;
    process->state = PROCESS_ZOMBIE;
    /* SIGCHLD still targets the parent's main task by identity. A sibling
     * thread can also be blocked in waitpid(), so notify the parent's dedicated
     * child-event channel. Do not broadcast on process_t itself: unrelated
     * waits may legitimately use that object as their channel. */
    if (process->parent_pid > 0) {
        int32_t parent_pid = process->parent_pid;
        process_t *parent = process_find(parent_pid);

        process_send_signal(parent_pid, SIGCHLD);
        if (parent) task_wake_all(&parent->waitpid_event);
    }
    task_wake_all(process);
    if (process->auto_reap) process_release(process);
}

static void process_task_exit(task_t *task, int32_t status) {
    process_t *process = task->process;

    if (!process) return;

    if (process->thread_count > 0) {
        /* Extra threads (SYS_THREAD_CREATE) are still running and share this
         * address space. Tearing it down now would leave their task_t
         * pointing at freed page tables the next time the scheduler picks
         * them. Defer the real cleanup to thread_on_exit(), which runs it
         * once the last thread has exited. */
        process->main_exited = 1;
        process->exit_status = status;
        process->task = NULL;
        return;
    }

    process_finish_exit(process, status);
}

int32_t process_launch(uint32_t entry_point, uint32_t user_stack,
                       address_space_t *address_space, const char *name,
                       uint32_t heap_base) {
    process_t *process;
    uint32_t used_count;

    if (!address_space) return -1;

    process = process_allocate();
    if (!process) return -1;

    process->entry_point = entry_point;
    process->user_stack = user_stack;
    process->address_space = address_space;
    process->heap_start = heap_base;
    process->heap_break = heap_base;
    process_copy_name(process->name, name);
    process->task = create_task(process_task_runner, process_task_exit,
                                address_space, process, 0, 0);
    if (!process->task) {
        process_release(process);
        return -1;
    }

    used_count = process_get_used_count();
    if (used_count > peak_process_count) peak_process_count = used_count;
    return process->pid;
}

int32_t process_fork(process_t *parent, address_space_t *space,
                     const fork_frame_t *frame) {
    process_t *child;
    uint32_t used_count;

    if (!parent || !space || !frame) return -1;

    child = process_allocate();   /* parent_pid set to the current (forking) pid */
    if (!child) return -1;

    child->address_space = space;
    child->entry_point = parent->entry_point;
    child->user_stack = parent->user_stack;
    child->heap_start = parent->heap_start;
    child->heap_break = parent->heap_break;
    /* Inherit the mmap-region reservations (the pages themselves are COW). */
    memcpy(child->ext_map, parent->ext_map, sizeof(child->ext_map));
    process_copy_name(child->name, parent->name);

    for (int i = 0; i < PROCESS_CWD_MAX; i++) child->cwd[i] = parent->cwd[i];

    /* Inherit signal dispositions, but start with none pending. */
    child->sig_trampoline = parent->sig_trampoline;
    for (int i = 0; i < NSIG; i++) child->sig_handler[i] = parent->sig_handler[i];
    child->sig_pending = 0;
    child->in_signal = 0;

    child->fork_frame = *frame;

    /* Inherit the environment. */
    child->env_count = parent->env_count;
    for (int i = 0; i < parent->env_count; i++) {
        memcpy(child->env_keys[i], parent->env_keys[i], ENV_KEY_MAX);
        memcpy(child->env_vals[i], parent->env_vals[i], ENV_VAL_MAX);
    }

    /* Duplicate the open file table before the child can run. */
    syscall_copy_user_files(parent, child);

    /* Inherit the standard streams too. Unix fork duplicates the whole
     * descriptor table, and syscall_copy_user_files only covers fds >= 3;
     * without this a child of a process whose stdout was redirected would
     * write to the terminal instead of the redirect target. Each stream takes
     * its own reference / pipe-end count, released by process_finish_exit. */
    child->stdout_node = parent->stdout_node;
    child->stdout_offset = parent->stdout_offset;
    if (child->stdout_node) open_fs(child->stdout_node);
    child->stdin_node = parent->stdin_node;
    child->stdin_offset = parent->stdin_offset;
    if (child->stdin_node) open_fs(child->stdin_node);
    child->stdout_pipe = parent->stdout_pipe;
    if (child->stdout_pipe) pipe_ref_write(child->stdout_pipe);
    child->stdin_pipe = parent->stdin_pipe;
    if (child->stdin_pipe) pipe_ref_read(child->stdin_pipe);

    child->task = create_task(fork_child_entry, process_task_exit, space, child, 0, 0);
    if (!child->task) {
        /* Release everything inherited above; the child never runs. */
        if (child->stdout_node) close_fs(child->stdout_node);
        if (child->stdin_node) close_fs(child->stdin_node);
        if (child->stdout_pipe) pipe_close_write(child->stdout_pipe);
        if (child->stdin_pipe) pipe_close_read(child->stdin_pipe);
        syscall_close_user_files(child);
        process_release(child);
        return -1;
    }

    used_count = process_get_used_count();
    if (used_count > peak_process_count) peak_process_count = used_count;
    return child->pid;
}

int process_exec_reset(address_space_t *new_space, uint32_t heap_base,
                       const char *name) {
    process_t *process = process_get_current();
    task_t *task = task_get_current();
    address_space_t *old;

    if (!process || !task || !new_space) return -1;

    /* Refuse exec while sibling threads share this address space. exec frees
     * the old address space below; a still-running SYS_THREAD_CREATE thread
     * holds a task_t->address_space pointer into it and would fault on freed
     * page tables the next time it is scheduled (the same use-after-free class
     * as the deferred-teardown bug fixed in process_task_exit). Real Unix exec
     * atomically terminates every other thread of the process; miniOS has no
     * mechanism to force-stop an arbitrary running task safely, so the
     * conservative, correctness-preserving choice is to fail the exec and
     * leave the caller running. The caller (sys_execv) reports -1 and discards
     * the freshly built image, so nothing is torn down. */
    if (process->thread_count > 0) return -1;

    /* Switch the running task to the freshly loaded image, then free the old. */
    old = process->address_space;
    process->address_space = new_space;
    task->address_space = new_space;
    paging_activate_address_space(new_space);
    if (old) paging_destroy_user_address_space(old);

    process->heap_start = heap_base;
    process->heap_break = heap_base;
    memset(process->ext_map, 0, sizeof(process->ext_map));  /* fresh image */
    process_copy_name(process->name, name);

    /* exec resets caught signals to their default disposition. */
    for (int i = 0; i < NSIG; i++) process->sig_handler[i] = 0;
    process->sig_pending = 0;
    process->in_signal = 0;
    process->sig_trampoline = 0;
    return 0;
}

int32_t process_wait(int32_t pid) {
    int32_t current_pid = process_get_current_pid();
    process_t *process;
    int32_t status;
    uint32_t flags = save_irq_disable();

    /* Find and validate the target while the process table is stable. Another
     * thread in this parent may reap the same child while this waiter blocks,
     * so the cached slot must never be trusted again without revalidating its
     * identity and parent after every wake. */
    process = process_find(pid);
    if (!process || process->parent_pid != current_pid) {
        restore_irq(flags);
        return -1;
    }

    while (process->state == PROCESS_RUNNING) {
        task_block_killable(process);
        if (process->state == PROCESS_UNUSED || process->pid != pid ||
            process->parent_pid != current_pid) {
            restore_irq(flags);
            return -1;
        }
    }

    if (process->state != PROCESS_ZOMBIE || process->pid != pid ||
        process->parent_pid != current_pid) {
        restore_irq(flags);
        return -1;
    }

    status = process->exit_status;
    process_release(process);
    restore_irq(flags);
    return status;
}

int32_t process_waitpid(int32_t pid, int32_t *status_out, int nohang) {
    process_t *current = process_get_current();
    int32_t current_pid;
    uint32_t flags;

    if (!current) return -1;
    current_pid = current->pid;
    flags = save_irq_disable();

    while (1) {
        process_t *zombie = NULL;
        int has_child = 0;

        for (int i = 0; i < MAX_PROCESSES; i++) {
            process_t *child = &processes[i];

            if (child->state == PROCESS_UNUSED ||
                child->parent_pid != current_pid) {
                continue;
            }
            if (pid > 0 && child->pid != pid) continue;

            has_child = 1;
            if (child->state == PROCESS_ZOMBIE) { zombie = child; break; }
        }

        if (zombie) {
            int32_t rpid = zombie->pid;
            int32_t status = zombie->exit_status;
            process_release(zombie);
            restore_irq(flags);
            if (status_out) *status_out = status;
            return rpid;
        }
        if (!has_child) { restore_irq(flags); return -1; }
        if (nohang) { restore_irq(flags); return 0; }

        /* Block on the parent's child-event channel. SIGCHLD still wakes the
         * main task by identity, while this channel reaches sibling waiters too. */
        task_block_killable(&current->waitpid_event);
    }
}

process_t *process_get_current(void) {
    task_t *task = task_get_current();
    return task ? task->process : NULL;
}

int32_t process_get_current_pid(void) {
    process_t *process = process_get_current();
    return process ? process->pid : 0;
}

uint32_t process_get_running_count(void) {
    uint32_t count = 0;

    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].state == PROCESS_RUNNING) count++;
    }
    return count;
}

uint32_t process_get_zombie_count(void) {
    uint32_t count = 0;

    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (processes[i].state == PROCESS_ZOMBIE) count++;
    }
    return count;
}

uint32_t process_get_peak_count(void) {
    return peak_process_count;
}

uint32_t process_snapshot(process_info_t *out, uint32_t max) {
    uint32_t flags = save_irq_disable();
    uint32_t count = 0;

    if (!out) { restore_irq(flags); return 0; }

    for (int i = 0; i < MAX_PROCESSES && count < max; i++) {
        if (processes[i].state == PROCESS_UNUSED) continue;
        out[count].pid = processes[i].pid;
        out[count].parent_pid = processes[i].parent_pid;
        out[count].state = processes[i].state;
        process_copy_name(out[count].name, processes[i].name);
        count++;
    }

    restore_irq(flags);
    return count;
}

int process_detach(int32_t pid) {
    uint32_t flags = save_irq_disable();
    process_t *process = process_find(pid);

    if (!process) { restore_irq(flags); return -1; }

    /* If it has already exited, reap it now; otherwise let it self-release. */
    if (process->state == PROCESS_ZOMBIE) {
        process_release(process);
    } else {
        process->parent_pid = 0;
        process->auto_reap = 1;
    }

    restore_irq(flags);
    return 0;
}

void process_redirect(int32_t pid, struct fs_node *out_node,
                      uint32_t out_offset, struct fs_node *in_node) {
    uint32_t flags = save_irq_disable();
    process_t *process = process_find(pid);

    if (process) {
        /* Hold a VFS reference for as long as a stream points at the node.
         * Without it the file could be unlinked and its node freed while this
         * process still writes through the pointer -- a use-after-free that
         * would call a function pointer read out of freed heap memory.
         * process_finish_exit() drops these references. */
        if (process->stdout_node) close_fs(process->stdout_node);
        process->stdout_node = out_node;
        if (out_node) open_fs(out_node);
        process->stdout_offset = out_offset;

        if (process->stdin_node) close_fs(process->stdin_node);
        process->stdin_node = in_node;
        if (in_node) open_fs(in_node);
        process->stdin_offset = 0;
    }

    restore_irq(flags);
}

void process_set_cwd(int32_t pid, const char *cwd) {
    uint32_t flags = save_irq_disable();
    process_t *process = process_find(pid);

    if (process && cwd) {
        int i = 0;
        while (i < PROCESS_CWD_MAX - 1 && cwd[i]) {
            process->cwd[i] = cwd[i];
            i++;
        }
        process->cwd[i] = '\0';
    }

    restore_irq(flags);
}

void process_pipe(int32_t pid, struct pipe *out_pipe, struct pipe *in_pipe) {
    uint32_t flags = save_irq_disable();
    process_t *process = process_find(pid);

    if (process) {
        if (out_pipe) process->stdout_pipe = out_pipe;
        if (in_pipe)  process->stdin_pipe = in_pipe;
    }

    restore_irq(flags);
}

int process_kill(int32_t pid) {
    uint32_t flags = save_irq_disable();
    process_t *process = process_find(pid);
    int running = process && process->state == PROCESS_RUNNING;

    restore_irq(flags);
    if (!running) return -1;
    process_request_kill(pid);
    return 0;
}

int process_send_signal(int32_t pid, int signum) {
    uint32_t flags = save_irq_disable();
    process_t *process = process_find(pid);
    int ok = process && signum > 0 && signum < NSIG;

    if (ok) {
        process->sig_pending |= (1u << signum);
        /* Nudge THIS process's task so it returns toward user mode for
         * delivery. It must be woken by identity, not via
         * task_wake_one(wait_channel): unrelated tasks share wait channels
         * (e.g. the shell blocks on a child's process_t in process_wait while
         * that child blocks on the same process_t in process_waitpid), so
         * waking "some waiter" on the channel can wake the wrong task and
         * leave this one blocked forever. Every blocking site re-checks its
         * condition in a loop, so a spurious wake is harmless. */
        if (process->task && process->task->state == TASK_BLOCKED) {
            task_wake_task(process->task);
        }
    }

    restore_irq(flags);
    return ok ? 0 : -1;
}

void process_pause(void) {
    uint32_t flags = save_irq_disable();
    process_t *process = process_get_current();

    if (process) {
        /* Sleep until a signal arrives; process_send_signal wakes us. */
        while (process->sig_pending == 0) {
            task_block_killable(process);
        }
    }

    restore_irq(flags);
}

/* First thing a new thread runs (in kernel mode): jump to its user entry point
 * on its own user stack, inside the shared address space. */
static void thread_trampoline(void) {
    task_t *task = task_get_current();

    if (!task) task_exit(-1);
    enter_user_mode(task->user_entry, task->user_stack);
}

/* Runs when a thread task exits: drop the live-thread count and wake any joiner.
 * Normally (the main task still running, or already exited with no threads
 * left) this must NOT free the shared address space or reap the process --
 * that belongs to process_task_exit(). The one exception: if the main task
 * already exited while this thread was still running (process->main_exited,
 * deferred by process_task_exit above), this is the last task sharing the
 * address space to stop, so it must finish the teardown that was deferred. */
static void thread_on_exit(task_t *task, int32_t status) {
    process_t *process = task->process;

    (void)status;
    if (!process) return;
    if (process->thread_count > 0) process->thread_count--;
    task_wake_all(&process->thread_count);
    if (process->thread_count == 0 && process->main_exited) {
        process_finish_exit(process, process->exit_status);
    }
}

int32_t process_thread_create(uint32_t entry, uint32_t stack_top) {
    uint32_t flags = save_irq_disable();
    process_t *process = process_get_current();
    task_t *task;

    if (!process || !process->address_space || entry == 0 || stack_top == 0) {
        restore_irq(flags);
        return -1;
    }

    process->thread_count++;
    task = create_task(thread_trampoline, thread_on_exit,
                       process->address_space, process, entry, stack_top);
    if (!task) {
        process->thread_count--;
        restore_irq(flags);
        return -1;
    }

    restore_irq(flags);
    return (int32_t)task->id;
}

void process_thread_join(void) {
    uint32_t flags = save_irq_disable();
    process_t *process = process_get_current();

    if (process) {
        while (process->thread_count > 0) {
            task_block_killable(&process->thread_count);
        }
    }

    restore_irq(flags);
}

/* --- mmap-region page allocator ---------------------------------------------
 * The mmap region (USER_EXT_BASE..USER_EXT_TOP) is managed as USER_EXT_PAGES
 * page-sized slots in a per-process bitmap: SYS_MMAP reserves the first free
 * run and SYS_MUNMAP releases one, so freed addresses are handed out again.
 * The page-fault handler consults the bitmap, so only reserved pages are
 * demand-paged; touching a freed page faults like any other bad pointer. */

static int ext_bit_get(const process_t *proc, uint32_t index) {
    return (proc->ext_map[index >> 5] >> (index & 31)) & 1;
}

static void ext_bit_set(process_t *proc, uint32_t index, int value) {
    if (value) proc->ext_map[index >> 5] |= 1U << (index & 31);
    else       proc->ext_map[index >> 5] &= ~(1U << (index & 31));
}

int process_ext_reserved(const process_t *proc, uint32_t vaddr) {
    if (!proc || vaddr < USER_EXT_BASE || vaddr >= USER_EXT_TOP) return 0;
    return ext_bit_get(proc, (vaddr - USER_EXT_BASE) >> 12);
}

int32_t process_ext_alloc(process_t *proc, uint32_t npages) {
    uint32_t flags, run = 0;

    if (!proc || npages == 0 || npages > USER_EXT_PAGES) return 0;

    /* Threads share this bitmap, so keep the scan-and-reserve atomic. */
    flags = save_irq_disable();
    for (uint32_t i = 0; i < USER_EXT_PAGES; i++) {
        run = ext_bit_get(proc, i) ? 0 : run + 1;
        if (run == npages) {
            uint32_t first = i + 1 - npages;
            for (uint32_t j = first; j <= i; j++) ext_bit_set(proc, j, 1);
            restore_irq(flags);
            return (int32_t)(USER_EXT_BASE + (first << 12));
        }
    }
    restore_irq(flags);
    return 0;   /* no free run is large enough */
}

int process_ext_free(process_t *proc, uint32_t vaddr, uint32_t npages) {
    uint32_t flags, first;

    if (!proc || npages == 0 || (vaddr & 0xFFFU)) return -1;
    if (vaddr < USER_EXT_BASE || vaddr >= USER_EXT_TOP) return -1;
    first = (vaddr - USER_EXT_BASE) >> 12;
    if (npages > USER_EXT_PAGES - first) return -1;

    /* Refuse partial frees: every page in the range must be reserved, so a
     * double free (or a bad base) fails cleanly without changing anything. */
    flags = save_irq_disable();
    for (uint32_t i = first; i < first + npages; i++) {
        if (!ext_bit_get(proc, i)) {
            restore_irq(flags);
            return -1;
        }
    }
    for (uint32_t i = first; i < first + npages; i++) ext_bit_set(proc, i, 0);
    restore_irq(flags);
    return 0;
}

/* Bounded copy into a fixed env field (always null-terminated). */
static void env_copy(char *dst, const char *src, uint32_t max) {
    uint32_t i = 0;
    if (max == 0) return;
    while (i < max - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

int process_setenv(const char *key, const char *val) {
    process_t *process = process_get_current();

    if (!process || !key || !val || key[0] == '\0') return -1;
    if (strlen(key) >= ENV_KEY_MAX || strlen(val) >= ENV_VAL_MAX) return -1;

    for (int i = 0; i < process->env_count; i++) {
        if (strcmp(process->env_keys[i], key) == 0) {
            env_copy(process->env_vals[i], val, ENV_VAL_MAX);
            return 0;
        }
    }
    if (process->env_count >= ENV_MAX) return -1;
    env_copy(process->env_keys[process->env_count], key, ENV_KEY_MAX);
    env_copy(process->env_vals[process->env_count], val, ENV_VAL_MAX);
    process->env_count++;
    return 0;
}

int process_getenv(const char *key, char *buf, uint32_t size) {
    process_t *process = process_get_current();

    if (!process || !key || !buf || size == 0) return -1;
    for (int i = 0; i < process->env_count; i++) {
        if (strcmp(process->env_keys[i], key) == 0) {
            env_copy(buf, process->env_vals[i], size);
            return (int)strlen(buf);
        }
    }
    return -1;
}

int process_has_sighandler(int32_t pid, int signum) {
    uint32_t flags = save_irq_disable();
    process_t *process = process_find(pid);
    int has = process && signum > 0 && signum < NSIG &&
              process->sig_handler[signum] > 1;

    restore_irq(flags);
    return has;
}

void process_request_kill(int32_t pid) {
    process_t *proc;
    /* Mark and wake every task of a process, not just proc->task. Two
     * reasons: a process may own several tasks (SYS_THREAD_CREATE), and a
     * blocked task cannot be killed by the per-tick check below -- it only
     * ever runs the few instructions between waking and re-blocking, with
     * interrupts disabled, so no tick lands on it. task_kill_blocked() flags
     * them so each leaves from inside its wait instead of looping back. */
    proc = process_find(pid);
    if (proc) task_kill_blocked(proc);
}

void process_account_tick(void) {
    process_t *process = process_get_current();
    if (process) process->cpu_ticks++;
}

uint32_t process_get_cpu_ticks(void) {
    process_t *process = process_get_current();
    return process ? process->cpu_ticks : 0;
}

void process_check_alarms(uint32_t current_tick) {
    for (int i = 0; i < MAX_PROCESSES; i++) {
        process_t *process = &processes[i];

        if (process->state == PROCESS_RUNNING && process->alarm_active &&
            (int32_t)(current_tick - process->alarm_tick) >= 0) {
            process->alarm_active = 0;
            process->alarm_tick = 0;
            process_send_signal(process->pid, SIGALRM);
        }
    }
}

void process_check_kill(void) {
    /* task_kill_blocked() marks every task of every requested process. Using
     * the current task's flag instead of one global target keeps simultaneous
     * requests independent and naturally covers every thread in a process. */
    if (task_kill_pending())
        task_exit(TASK_KILL_STATUS);  /* EOI was already sent by irq_handler */
}
