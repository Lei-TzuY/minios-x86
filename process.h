#ifndef PROCESS_H
#define PROCESS_H

#include <stdint.h>
#include "paging.h"

#define MAX_PROCESSES 16
/* Pages in the mmap region: (USER_EXT_TOP - USER_EXT_BASE) / 4096, see
 * elf_loader.h. Sized here so the reservation bitmap can live in process_t. */
#define USER_EXT_PAGES 1024
#define PROCESS_NAME_MAX 16
#define PROCESS_CWD_MAX 128
#define ENV_MAX      8     /* environment variables per process */
#define ENV_KEY_MAX  24
#define ENV_VAL_MAX  64
#define NSIG 32
#define SIGINT  2
#define SIGKILL 9   /* uncatchable: always force-terminates */
#define SIGUSR1 10
#define SIGALRM 14  /* delivered when an alarm timer expires */
#define SIGTERM 15
#define SIGCHLD 17  /* sent to a parent when a child exits; default: ignore */
#define SIGCONT 18  /* resume a stopped process; default: ignore */
#define SIGSTOP 19  /* uncatchable: suspend the process until SIGCONT */

struct task;
struct fs_node;
struct pipe;

typedef enum process_state {
    PROCESS_UNUSED = 0,
    PROCESS_RUNNING,
    PROCESS_ZOMBIE,
} process_state_t;

/* Saved user-mode registers a forked child resumes from. Field order matches
 * the offsets read by enter_user_mode_iret in usermode_s.s. */
typedef struct fork_frame {
    uint32_t eip, eflags, useresp;
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp;
} fork_frame_t;

typedef struct process {
    int32_t pid;
    int32_t parent_pid;
    int32_t exit_status;
    uint32_t entry_point;
    uint32_t user_stack;
    uint32_t slot;
    uint8_t auto_reap;
    process_state_t state;
    address_space_t *address_space;
    struct task *task;
    char name[PROCESS_NAME_MAX];
    uint32_t heap_start;   /* fixed: page above the loaded ELF image */
    uint32_t heap_break;   /* current program break (grows via SYS_SBRK) */
    /* mmap-region reservation bitmap, one bit per page: set = handed out by
     * SYS_MMAP (and eligible for demand paging), clear = free/unmapped. */
    uint32_t ext_map[USER_EXT_PAGES / 32];
    /* Standard I/O redirection. NULL node means the default device
     * (terminal for stdout, keyboard for stdin). */
    struct fs_node *stdout_node;
    uint32_t        stdout_offset;
    struct fs_node *stdin_node;
    uint32_t        stdin_offset;
    /* Pipeline endpoints. Take priority over the file/device targets above:
     * a write end on stdout, a read end on stdin. */
    struct pipe    *stdout_pipe;
    struct pipe    *stdin_pipe;
    char            cwd[PROCESS_CWD_MAX];  /* working dir for relative paths */
    /* Signal handling. sig_handler[s]: 0 = default (terminate), 1 = ignore,
     * else a user-space handler address. */
    uint32_t        sig_pending;
    uint32_t        sig_handler[NSIG];
    uint32_t        sig_trampoline;  /* user sigreturn trampoline address */
    uint8_t         in_signal;       /* a handler is currently running */
    uint8_t         stopped;         /* job control: suspended by SIGSTOP */
    uint32_t        thread_count;    /* live threads sharing this address space */
    /* Set when the main task has called exit()/returned while extra threads
     * (SYS_THREAD_CREATE) are still running. Full teardown (address space,
     * open files, zombie transition) is deferred until thread_count reaches
     * zero -- see process_task_exit()/thread_on_exit() in process.c. Without
     * this, exiting while a sibling thread is still scheduled would destroy
     * the address space out from under it. */
    uint8_t         main_exited;
    /* Dedicated wait channel for threads blocked in waitpid(). Its address,
     * not its value, is the synchronization identity. Keeping it distinct
     * from process_t prevents child-exit notification from waking unrelated
     * tasks that happen to wait on the process object itself. */
    uint8_t         waitpid_event;
    uint8_t         alarm_active;    /* alarm_tick is meaningful even when zero */
    uint32_t        alarm_tick;      /* timer tick to raise SIGALRM */
    uint32_t        cpu_ticks;       /* timer ticks spent running this process */
    fork_frame_t    fork_frame;      /* user context a forked child resumes */
    /* Environment variables: inherited across fork, preserved across exec. */
    char            env_keys[ENV_MAX][ENV_KEY_MAX];
    char            env_vals[ENV_MAX][ENV_VAL_MAX];
    uint8_t         env_count;
} process_t;

/* Read-only snapshot of one process, copied out for `ps`. */
typedef struct process_info {
    int32_t pid;
    int32_t parent_pid;
    process_state_t state;
    char name[PROCESS_NAME_MAX];
} process_info_t;

int32_t process_launch(uint32_t entry_point, uint32_t user_stack,
                       address_space_t *address_space, const char *name,
                       uint32_t heap_base);

/* Fork `parent` into a new process running in the cloned address space `space`,
 * inheriting cwd/heap/signals/open files. The child resumes from `frame` with
 * eax = 0. Returns the child pid, or -1 on failure. */
int32_t process_fork(process_t *parent, address_space_t *space,
                     const fork_frame_t *frame);

/* Replace the current process's image with `new_space` (from exec): swap and
 * free the old address space, reset heap and signal dispositions, set the new
 * program name. The pid, parent, cwd and open files are preserved. */
int process_exec_reset(address_space_t *new_space, uint32_t heap_base,
                       const char *name);
int32_t process_wait(int32_t pid);
/* Reap a child: pid > 0 waits for that child, pid == -1 for any child. Writes
 * the exit status through `status_out` (if non-NULL). With nohang set, returns
 * 0 immediately when a matching child exists but none has exited yet; returns
 * the reaped child's pid on success, or -1 if there is no such child. */
int32_t process_waitpid(int32_t pid, int32_t *status_out, int nohang);
int32_t process_get_current_pid(void);
process_t *process_get_current(void);
uint32_t process_get_running_count(void);
uint32_t process_get_zombie_count(void);
uint32_t process_get_peak_count(void);

/* Copy up to `max` live process records into `out`; returns the count written. */
uint32_t process_snapshot(process_info_t *out, uint32_t max);
/* Detach a process so it self-releases on exit instead of waiting to be reaped.
 * Used for background (`&`) jobs the shell will not wait on. */
int process_detach(int32_t pid);
/* Validated kill for a running process (shell `kill`). Returns 0, or -1 if no
 * such running process exists. */
int process_kill(int32_t pid);

/* Redirect a not-yet-running process's standard I/O. A NULL node leaves the
 * corresponding stream on its default device. Must be called after launch but
 * before the child first runs (the shell does this while still scheduled). */
void process_redirect(int32_t pid, struct fs_node *out_node,
                      uint32_t out_offset, struct fs_node *in_node);

/* Attach pipeline endpoints to a not-yet-running process (NULL leaves a stream
 * unchanged). The process takes ownership of the given ends and closes them on
 * exit. Like process_redirect, call after launch but before the child runs. */
void process_pipe(int32_t pid, struct pipe *out_pipe, struct pipe *in_pipe);

/* Set a process's working directory (used for resolving relative paths in its
 * file syscalls). The shell calls this so children inherit its cwd. */
void process_set_cwd(int32_t pid, const char *cwd);

/* Post a signal to a process (sets it pending; woken if blocked). Returns 0 on
 * success, -1 if no such process exists. */
int process_send_signal(int32_t pid, int signum);
/* Block the current process until a signal becomes pending (pause()). */
void process_pause(void);

/* Threads: create a new task that shares the current process's address space
 * and open files, running `entry` on the user stack `stack_top`. Threads see
 * each other's memory directly (no copy-on-write). Returns the thread id, or -1.
 * Joining before the process exits is not required for correctness: if the
 * main task exits first, process_task_exit() defers freeing the shared
 * address space until the last remaining thread exits (see thread_count /
 * main_exited in process_t), so a still-running sibling never sees it
 * disappear out from under it. A parent's wait()/waitpid() on this process
 * blocks for that entire deferred period. */
int32_t process_thread_create(uint32_t entry, uint32_t stack_top);
/* Block until every thread of the current process has exited. */
void process_thread_join(void);

/* Page-granular allocator for the process's mmap region (SYS_MMAP/SYS_MUNMAP).
 * Alloc reserves the first free run of `npages` and returns its base address
 * (0 if no run fits). Free releases a reserved run; every page in the range
 * must currently be reserved, else it fails with -1 and changes nothing.
 * Reserved tells the page-fault handler whether `vaddr` may be demand-paged. */
int32_t process_ext_alloc(process_t *proc, uint32_t npages);
int process_ext_free(process_t *proc, uint32_t vaddr, uint32_t npages);
int process_ext_reserved(const process_t *proc, uint32_t vaddr);

/* Environment variables for the current process. setenv adds or replaces a
 * key; getenv copies the value into `buf` (always null-terminated) and returns
 * its length, or -1 if the key is unset. Both return -1 on bad arguments. */
int process_setenv(const char *key, const char *val);
int process_getenv(const char *key, char *buf, uint32_t size);
/* True if the process has a real (catchable) handler installed for `signum`. */
int  process_has_sighandler(int32_t pid, int signum);

/* Mark every task of a process for termination and wake its blocked tasks.
 * Runnable tasks leave on their next timer tick. Safe from interrupt context. */
void process_request_kill(int32_t pid);
/* Called from timer_callback: kill the current task if it has been marked. */
void process_check_kill(void);
/* Called each timer tick: raise SIGALRM on processes whose alarm has expired. */
void process_check_alarms(uint32_t current_tick);

/* Charge the running timer tick to the current process (CPU accounting). Safe
 * to call from the timer interrupt; a no-op when a kernel task is current. */
void process_account_tick(void);
/* CPU ticks consumed by the current process (0 if none is running). */
uint32_t process_get_cpu_ticks(void);

#endif
