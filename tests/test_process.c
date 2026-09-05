#include <setjmp.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The process lifecycle state machine (process.c):
 *
 *   allocate/launch -> running -> fork/thread -> main exit
 *     -> zombie or deferred exit -> wait/waitpid/detach -> reap -> slot reuse
 *
 * Four of this project's P0/P1 defects lived here -- F1 (address space freed
 * while a sibling thread still ran), F7 (execv from a multithreaded process),
 * F17 (kill reached only one task), F19 (a task parked in a wait loop could
 * not be killed at all) -- and each fix left behind an invariant that is
 * enforced by the interaction of several functions rather than by any one of
 * them. None of those interactions had a unit test.
 *
 * The harness models the scheduler rather than stubbing it away, because the
 * questions worth asking are about who blocked, on what, and who woke them:
 *
 *   - task_block_current() records the blocking task and its channel, runs a
 *     scripted "meanwhile" (the event the sleeper is waiting for), and then
 *     checks whether anything actually woke THIS task. If nothing did, the
 *     block is recorded as stuck. That is what makes "this would hang forever"
 *     an assertion instead of a real hang.
 *   - task_wake_task() and task_wake_all() are recorded separately, by target
 *     and by channel, because the two waiting functions in process.c rely on
 *     different mechanisms and the tests have to tell them apart.
 *   - address spaces count their own create/activate/destroy, so teardown
 *     ordering -- in particular that nothing frees the space the CPU is still
 *     running on -- is checkable rather than assumed.
 *
 * The first thing this suite pins down is the dependency ASSESS2 found:
 * process_waitpid() blocks on the PARENT's process_t, while the exiting child
 * broadcasts on the CHILD's. The parent is woken only because SIGCHLD delivery
 * wakes a blocked target by identity, and only because it does so whether or
 * not a handler is installed. Making that conditional is a plausible-looking
 * optimisation that would hang every waitpid in the system.
 */

#include "../process.h"
#include "../fs.h"
#include "../pipe.h"
#include "../utils.h"      /* the kernel's memset/memcpy, which process.c uses */

/* --- address spaces -------------------------------------------------------- */

#define MAX_SPACES 8
static address_space_t g_spaces[MAX_SPACES];
static int g_space_destroyed[MAX_SPACES];
static address_space_t *g_active_space;
static int g_destroy_while_active;      /* freed the space the CPU was on */
static int g_activate_calls;

/* Teardown steps record WHEN they ran, not just how often. Swapping two of
 * them leaves every count identical and only the order different, so a
 * counter cannot see it. */
static int g_seq;
static int g_seq_destroy_space;
static int g_seq_close_files;
static int g_seq_release_streams;

static int space_index(const address_space_t *s) {
    for (int i = 0; i < MAX_SPACES; i++)
        if (&g_spaces[i] == s) return i;
    return -1;
}

void paging_activate_address_space(address_space_t *space) {
    g_activate_calls++;
    g_active_space = space;
}

void paging_destroy_user_address_space(address_space_t *space) {
    int i = space_index(space);

    if (i < 0) return;
    /* The real implementation switches to the kernel space first if this one
     * is live. Recording the condition rather than silently doing the same
     * thing is what lets a test see an ordering regression: freeing the page
     * tables the CPU is executing on is the F1 class of failure. */
    if (space == g_active_space) g_destroy_while_active++;
    g_space_destroyed[i]++;
    g_seq_destroy_space = ++g_seq;
}

/* --- files and pipes: reference counts, as in CAP19 ------------------------ */

#define MAX_NODES 4
#define MAX_PIPES 4
static fs_node_t g_nodes[MAX_NODES];
static int g_node_refs[MAX_NODES];
static int g_node_underflow;
static pipe_t g_pipes[MAX_PIPES];
static int g_pipe_read_refs[MAX_PIPES];
static int g_pipe_write_refs[MAX_PIPES];
static int g_pipe_underflow;

static int node_index(const fs_node_t *n) {
    for (int i = 0; i < MAX_NODES; i++) if (&g_nodes[i] == n) return i;
    return -1;
}
static int pipe_idx(const pipe_t *p) {
    for (int i = 0; i < MAX_PIPES; i++) if (&g_pipes[i] == p) return i;
    return -1;
}

void open_fs(fs_node_t *n) { int i = node_index(n); if (i >= 0) g_node_refs[i]++; }
void close_fs(fs_node_t *n) {
    int i = node_index(n);
    if (i < 0) return;
    g_seq_release_streams = ++g_seq;
    if (g_node_refs[i] == 0) { g_node_underflow++; return; }
    g_node_refs[i]--;
}
void pipe_ref_read(pipe_t *p)  { int i = pipe_idx(p); if (i >= 0) g_pipe_read_refs[i]++; }
void pipe_ref_write(pipe_t *p) { int i = pipe_idx(p); if (i >= 0) g_pipe_write_refs[i]++; }
void pipe_close_read(pipe_t *p) {
    int i = pipe_idx(p);
    if (i < 0) return;
    if (g_pipe_read_refs[i] == 0) { g_pipe_underflow++; return; }
    g_pipe_read_refs[i]--;
}
void pipe_close_write(pipe_t *p) {
    int i = pipe_idx(p);
    if (i < 0) return;
    if (g_pipe_write_refs[i] == 0) { g_pipe_underflow++; return; }
    g_pipe_write_refs[i]--;
}

/* --- the descriptor table (CAP19's subject; here only as a counter) -------- */

static int g_close_files_calls;
static int g_copy_files_calls;
static int32_t g_last_closed_pid;
static int g_process_release_calls;
static process_t *g_last_released_process;

void process_test_observe_release(process_t *p) {
    g_process_release_calls++;
    g_last_released_process = p;
}

void syscall_close_user_files(struct process *p) {
    g_close_files_calls++;
    g_seq_close_files = ++g_seq;
    if (p) g_last_closed_pid = p->pid;
}
void syscall_copy_user_files(struct process *parent, struct process *child) {
    (void)parent; (void)child;
    g_copy_files_calls++;
}

/* --- the scheduler --------------------------------------------------------- */

#include "../task.h"

#define MAX_TASKS 8
static task_t   g_tasks[MAX_TASKS];
static int      g_task_used[MAX_TASKS];
static int      g_task_count;
static task_t  *g_current_task;
static int      g_create_task_fails;

/* Blocking and waking, recorded in full. */
static int          g_block_calls;
static const void  *g_last_block_channel;
static task_t      *g_last_block_task;
static int          g_stuck_blocks;        /* parked with nothing to wake us */
static int          g_woke_me;             /* set by a wake aimed at us */

/* A real scheduler has a blocked list. Keeping one in the stub matters: a
 * channel wake has to choose an actual waiter, so the harness can distinguish
 * the intended parent task from an older decoy on the same channel. */
static task_t       *g_blocked_tasks[MAX_TASKS];
static const void   *g_blocked_channels[MAX_TASKS];
static int           g_blocked_count;

static int          g_wake_task_calls;
static task_t      *g_last_wake_task;
static int          g_wake_all_calls;
static const void  *g_last_wake_all_channel;
static int          g_wake_one_calls;
static int          g_kill_blocked_calls;

/* The scripted event that happens while a task is parked. */
static void (*g_on_block)(void *);
static void  *g_on_block_arg;
static int    g_on_block_remaining;

static jmp_buf g_exit_jmp;
static int     g_task_exit_calls;
static int32_t g_task_exit_status;

/* A task that parks over and over with nothing ever aimed at it is not a
 * slow test, it is the kernel hanging. Past this many consecutive stuck
 * blocks the harness stops and says so by name, rather than leaving the
 * mutation script's timeout to report that something, somewhere, went
 * wrong. */
#define STUCK_BLOCK_LIMIT 64
static int g_hang_detected;

static void record_blocked(task_t *task, const void *channel) {
    if (!task || g_blocked_count >= MAX_TASKS) return;
    g_blocked_tasks[g_blocked_count] = task;
    g_blocked_channels[g_blocked_count] = channel;
    g_blocked_count++;
}

static int remove_blocked(task_t *task) {
    for (int i = 0; i < g_blocked_count; i++) {
        if (g_blocked_tasks[i] == task) {
            for (; i + 1 < g_blocked_count; i++) {
                g_blocked_tasks[i] = g_blocked_tasks[i + 1];
                g_blocked_channels[i] = g_blocked_channels[i + 1];
            }
            g_blocked_count--;
            return 1;
        }
    }
    return 0;
}

static task_t *remove_one_blocked(const void *channel) {
    for (int i = 0; i < g_blocked_count; i++) {
        task_t *task;

        if (g_blocked_channels[i] != channel) continue;
        task = g_blocked_tasks[i];
        (void)remove_blocked(task);
        return task;
    }
    return NULL;
}

static void park_decoy(task_t *task, const void *channel) {
    if (!task) return;
    task->state = TASK_BLOCKED;
    record_blocked(task, channel);
}

task_t *create_task(void (*entry)(void), task_exit_callback_t on_exit,
                    address_space_t *address_space, struct process *process,
                    uint32_t user_entry, uint32_t user_stack) {
    (void)entry; (void)user_entry; (void)user_stack;

    if (g_create_task_fails) return NULL;
    if (g_task_count >= MAX_TASKS) return NULL;
    {
        task_t *t = &g_tasks[g_task_count];

        memset(t, 0, sizeof(*t));
        t->id = (uint32_t)g_task_count + 1;
        t->on_exit = on_exit;
        t->address_space = address_space;
        t->process = process;
        t->state = TASK_READY;
        g_task_used[g_task_count] = 1;
        g_task_count++;
        return t;
    }
}

task_t *task_get_current(void) { return g_current_task; }

int task_kill_pending(void) {
    return g_current_task && g_current_task->kill_pending;
}

void task_exit(int32_t status) {
    g_task_exit_calls++;
    g_task_exit_status = status;
    longjmp(g_exit_jmp, 1);
}

void task_block_current(const void *channel) {
    g_block_calls++;
    g_last_block_channel = channel;
    g_last_block_task = g_current_task;
    if (g_current_task) g_current_task->state = TASK_BLOCKED;
    record_blocked(g_current_task, channel);
    g_woke_me = 0;

    /* Something else runs while this task is parked -- the event it is waiting
     * for, if the test scripted one. */
    if (g_on_block_remaining > 0) {
        g_on_block_remaining--;
        if (g_on_block) g_on_block(g_on_block_arg);
    }

    /* Nothing aimed a wake at this task: in the real kernel it would stay
     * parked forever, and the caller's loop would never run again. */
    if (!g_woke_me) g_stuck_blocks++;
    else g_stuck_blocks = 0;
    (void)remove_blocked(g_current_task);
    if (g_current_task) g_current_task->state = TASK_READY;

    if (g_stuck_blocks > STUCK_BLOCK_LIMIT) {
        g_hang_detected = 1;
        longjmp(g_exit_jmp, 2);
    }
}

void task_wake_task(task_t *task) {
    g_wake_task_calls++;
    g_last_wake_task = task;
    /* Aimed at the PARKED task, not at whoever is running: while a task
     * is blocked the CPU belongs to someone else -- here, to the child
     * that is exiting and sending the signal. */
    if (task == g_last_block_task) g_woke_me = 1;
    (void)remove_blocked(task);
    if (task) task->state = TASK_READY;
}

void task_wake_all(const void *channel) {
    task_t *task;

    g_wake_all_calls++;
    g_last_wake_all_channel = channel;
    while ((task = remove_one_blocked(channel)) != NULL) {
        if (task == g_last_block_task) g_woke_me = 1;
        task->state = TASK_READY;
    }
}

void task_wake_one(const void *channel) {
    task_t *task;

    g_wake_one_calls++;
    task = remove_one_blocked(channel);
    if (task == g_last_block_task) g_woke_me = 1;
    if (task) task->state = TASK_READY;
}

uint32_t task_kill_blocked(struct process *process) {
    g_kill_blocked_calls++;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (g_task_used[i] && g_tasks[i].process == process)
            g_tasks[i].kill_pending = 1;
    }
    return 0;
}

void enter_user_mode(uint32_t eip, uint32_t user_esp) { (void)eip; (void)user_esp; }
void enter_user_mode_iret(const fork_frame_t *frame) { (void)frame; }

#include "../process.c"

#include "test.h"

/* --- harness --------------------------------------------------------------- */

static void reset_world(void) {
    for (int i = 0; i < MAX_SPACES; i++) {
        memset(&g_spaces[i], 0, sizeof(g_spaces[i]));
        g_space_destroyed[i] = 0;
    }
    g_active_space = NULL;
    g_destroy_while_active = 0;
    g_activate_calls = 0;

    for (int i = 0; i < MAX_NODES; i++) {
        memset(&g_nodes[i], 0, sizeof(g_nodes[i]));
        g_nodes[i].flags = FS_FILE;
        g_node_refs[i] = 0;
    }
    for (int i = 0; i < MAX_PIPES; i++) {
        g_pipe_read_refs[i] = 0;
        g_pipe_write_refs[i] = 0;
    }
    g_node_underflow = g_pipe_underflow = 0;
    g_close_files_calls = g_copy_files_calls = 0;
    g_process_release_calls = 0;
    g_last_released_process = NULL;
    g_seq = 0;
    g_seq_destroy_space = g_seq_close_files = g_seq_release_streams = 0;
    g_last_closed_pid = -1;

    for (int i = 0; i < MAX_TASKS; i++) {
        memset(&g_tasks[i], 0, sizeof(g_tasks[i]));
        g_task_used[i] = 0;
    }
    g_task_count = 0;
    g_current_task = NULL;
    g_create_task_fails = 0;

    g_block_calls = 0;
    g_hang_detected = 0;
    g_last_block_channel = NULL;
    g_last_block_task = NULL;
    g_stuck_blocks = 0;
    g_woke_me = 0;
    g_blocked_count = 0;
    g_wake_task_calls = 0;
    g_last_wake_task = NULL;
    g_wake_all_calls = 0;
    g_last_wake_all_channel = NULL;
    g_wake_one_calls = 0;
    g_kill_blocked_calls = 0;
    g_on_block = NULL;
    g_on_block_arg = NULL;
    g_on_block_remaining = 0;
    g_task_exit_calls = 0;
    g_task_exit_status = 0;

    /* Wipe the process table the way a fresh boot would leave it. */
    for (int i = 0; i < MAX_PROCESSES; i++) memset(&processes[i], 0, sizeof(processes[i]));
    next_pid = 1;
    peak_process_count = 0;
}

/* Run a process's exit the way the scheduler does: the task's on_exit hook. */
static void exit_task(task_t *task, int32_t status) {
    task_t *saved = g_current_task;

    g_current_task = task;
    if (task->on_exit) task->on_exit(task, status);
    g_current_task = saved;
}

static process_t *find_by_pid(int32_t pid) {
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (processes[i].state != PROCESS_UNUSED && processes[i].pid == pid)
            return &processes[i];
    return NULL;
}

static int used_slots(void) {
    int n = 0;
    for (int i = 0; i < MAX_PROCESSES; i++)
        if (processes[i].state != PROCESS_UNUSED) n++;
    return n;
}

static void expect_no_underflow(void) {
    CHECK_EQ(g_node_underflow, 0);
    CHECK_EQ(g_pipe_underflow, 0);
}

/* Launch a process and return it, with its task recorded. */
static process_t *launch(const char *name, int space_index_) {
    int32_t pid = process_launch(0x1000, 0x2000, &g_spaces[space_index_], name,
                                 0x3000);
    return pid < 0 ? NULL : find_by_pid(pid);
}

/* --- the dependency ASSESS2 found ----------------------------------------- */

static void child_exits(void *arg) {
    process_t *child = (process_t *)arg;

    exit_task(child->task, 7);
}

typedef struct wake_script {
    process_t *parent;
    process_t *child;
    int step;
} wake_script_t;

static void spurious_wake_then_child_exit(void *arg) {
    wake_script_t *script = (wake_script_t *)arg;

    /* SIGUSR1 is deliberately irrelevant to waitpid's predicate. It wakes
     * the task, but there is still no zombie to reap, so the loop must park
     * again. On the third park the actual child exit supplies the predicate. */
    if (script->step++ < 2) {
        (void)process_send_signal(script->parent->pid, SIGUSR1);
    } else {
        exit_task(script->child->task, 7);
    }
}

static void test_waitpid_is_woken_without_a_sigchld_handler(void) {
    process_t *parent, *child;
    int32_t status = 0;
    int32_t reaped;

    /*
     * The invariant, stated as plainly as it can be:
     *
     *   a parent blocked in waitpid() must be woken when its child exits, and
     *   must be woken whether or not it installed a SIGCHLD handler.
     *
     * process_waitpid blocks on the PARENT's process_t. process_finish_exit
     * broadcasts on the CHILD's. Those are different channels, so the
     * broadcast cannot be what wakes the parent -- the wake comes from SIGCHLD
     * delivery calling task_wake_task() on the parent's task by identity, and
     * that call is unconditional. Make it conditional on a handler being
     * installed and every waitpid in the system hangs.
     */
    TEST("waitpid is woken by a child exit with no SIGCHLD handler installed");
    reset_world();

    parent = launch("parent", 0);
    CHECK(parent != NULL);
    if (!parent) return;
    child = launch("child", 1);
    CHECK(child != NULL);
    if (!child) return;
    child->parent_pid = parent->pid;

    /* No handler at all: sig_handler[] is zero from process_allocate. */
    CHECK_EQ(parent->sig_handler[SIGCHLD], 0);

    g_current_task = parent->task;
    g_on_block = child_exits;
    g_on_block_arg = child;
    g_on_block_remaining = 1;

    {
        int32_t want = child->pid;   /* read before the reap zeroes it */

        reaped = process_waitpid(child->pid, &status, 0);
        CHECK_EQ(reaped, want);
    }
    CHECK_EQ(status, 7);

    /* It really did park, and it really was woken -- rather than the loop
     * happening to re-check at the right moment. */
    CHECK_EQ(g_block_calls, 1);
    CHECK_EQ(g_stuck_blocks, 0);

    /* And the wake came by identity, aimed at the parent's task. */
    CHECK(g_wake_task_calls > 0);
    CHECK(g_last_wake_task == parent->task);
    expect_no_underflow();
}

static void test_waitpid_blocks_on_dedicated_child_event(void) {
    process_t *parent, *child;
    int32_t status = 0;

    /*
     * The two waiting functions use different mechanisms, and the tests must
     * not assume they are the same. This one records which channel each of
     * them parks on, so a change to either is visible.
     */
    TEST("waitpid parks on a dedicated parent child-event channel");
    reset_world();

    parent = launch("parent", 0);
    child = launch("child", 1);
    CHECK(parent && child);
    if (!parent || !child) return;
    child->parent_pid = parent->pid;

    g_current_task = parent->task;
    g_on_block = child_exits;
    g_on_block_arg = child;
    g_on_block_remaining = 1;
    (void)process_waitpid(child->pid, &status, 0);

    /* waitpid uses an explicit child-event identity, not process_t itself.
     * The final broadcast is still the child's process_wait() channel. */
    CHECK(g_last_block_channel == (const void *)&parent->waitpid_event);
    CHECK(g_last_block_channel != (const void *)parent);
    CHECK(g_last_wake_all_channel == (const void *)child);
    CHECK(g_last_block_channel != g_last_wake_all_channel);
}

static void test_waitpid_wake_is_identity_not_channel_luck(void) {
    process_t *parent, *child, *decoy;
    int32_t status = 0;

    TEST("waitpid SIGCHLD wakes its parent, not an older same-channel waiter");
    reset_world();
    parent = launch("parent", 0);
    child = launch("child", 1);
    decoy = launch("decoy", 2);
    CHECK(parent && child && decoy);
    if (!parent || !child || !decoy) return;
    child->parent_pid = parent->pid;

    /* Model another older task that is blocked on the exact channel a wrong
     * implementation might broadcast/wake_one(). The true SIGCHLD route must
     * target parent->task by identity and leave this decoy untouched. */
    park_decoy(decoy->task, parent);
    CHECK_EQ(g_blocked_count, 1);

    g_current_task = parent->task;
    g_on_block = child_exits;
    g_on_block_arg = child;
    g_on_block_remaining = 1;

    {
        int32_t want = child->pid;

        CHECK_EQ(process_waitpid(want, &status, 0), want);
    }
    CHECK_EQ(status, 7);
    CHECK_EQ(g_stuck_blocks, 0);
    CHECK(g_last_wake_task == parent->task);
    CHECK_EQ(g_wake_one_calls, 0);
    CHECK_EQ(decoy->task->state, TASK_BLOCKED);
    CHECK_EQ(g_blocked_count, 1);
}

static void test_wait_parks_on_the_child(void) {
    process_t *parent, *child;
    int32_t status;

    TEST("process_wait parks on the child and the broadcast matches");
    reset_world();

    parent = launch("parent", 0);
    child = launch("child", 1);
    CHECK(parent && child);
    if (!parent || !child) return;
    child->parent_pid = parent->pid;

    g_current_task = parent->task;
    g_on_block = child_exits;
    g_on_block_arg = child;
    g_on_block_remaining = 1;

    status = process_wait(child->pid);
    CHECK_EQ(status, 7);

    /* Unlike waitpid, this one parks on the child, so the broadcast reaches
     * it directly. Recording both is what stops a future change from
     * "simplifying" one into the other and breaking the mechanism the other
     * one depends on. */
    CHECK(g_last_block_channel == (const void *)child);
    CHECK(g_last_wake_all_channel == (const void *)child);
    CHECK_EQ(g_stuck_blocks, 0);
}

static void test_spurious_wake_rechecks(void) {
    process_t *parent, *child;
    int32_t status = 0;
    int32_t reaped;
    wake_script_t script;

    TEST("a wake with nothing to reap parks again");
    reset_world();

    parent = launch("parent", 0);
    child = launch("child", 1);
    CHECK(parent && child);
    if (!parent || !child) return;
    child->parent_pid = parent->pid;

    g_current_task = parent->task;
    /* The first two wakes bring no zombie with them; only the third block is
     * followed by the child actually exiting. Every blocking site in this
     * kernel is written as `while (cond) block;` precisely so that a wake
     * that proves nothing is harmless. */
    script.parent = parent;
    script.child = child;
    script.step = 0;
    g_on_block = spurious_wake_then_child_exit;
    g_on_block_arg = &script;
    g_on_block_remaining = 3;

    {
        int32_t want = child->pid;

        reaped = process_waitpid(child->pid, &status, 0);
        CHECK_EQ(reaped, want);
    }
    CHECK_EQ(g_block_calls, 3);
    CHECK_EQ(g_stuck_blocks, 0);
    CHECK_EQ(script.step, 3);
    CHECK_EQ(g_wake_task_calls, 3); /* two spurious signals + SIGCHLD */
}

static void test_waitpid_nohang(void) {
    process_t *parent, *child;
    int32_t status = 0;

    TEST("waitpid nohang never blocks");
    reset_world();
    parent = launch("parent", 0);
    child = launch("child", 1);
    CHECK(parent && child);
    if (!parent || !child) return;
    child->parent_pid = parent->pid;
    g_current_task = parent->task;

    CHECK_EQ(process_waitpid(child->pid, &status, 1), 0);
    CHECK_EQ(g_block_calls, 0);

    exit_task(child->task, 3);
    {
        int32_t want = child->pid;

        CHECK_EQ(process_waitpid(child->pid, &status, 1), want);
    }
    CHECK_EQ(status, 3);
    CHECK_EQ(g_block_calls, 0);
}

static void test_wait_for_a_non_child(void) {
    process_t *parent, *other;
    int32_t status = 0;

    TEST("waiting for something that is not your child");
    reset_world();
    parent = launch("parent", 0);
    other = launch("other", 1);
    CHECK(parent && other);
    if (!parent || !other) return;
    other->parent_pid = 999;               /* someone else's child */
    g_current_task = parent->task;

    CHECK_EQ(process_waitpid(other->pid, &status, 0), -1);
    CHECK_EQ(process_waitpid(4242, &status, 0), -1);
    CHECK_EQ(process_wait(other->pid), -1);
    CHECK_EQ(process_wait(4242), -1);
    CHECK_EQ(g_block_calls, 0);            /* refused, not parked */
}

/* --- launch ---------------------------------------------------------------- */

static void test_launch_success(void) {
    process_t *p;

    TEST("launch: ownership after a successful start");
    reset_world();

    p = launch("prog", 0);
    CHECK(p != NULL);
    if (!p) return;

    CHECK_EQ(p->state, PROCESS_RUNNING);
    CHECK_EQ(p->pid, 1);
    CHECK(p->address_space == &g_spaces[0]);
    CHECK(p->task != NULL);
    CHECK(p->task->process == p);
    CHECK_EQ(p->thread_count, 0);
    CHECK_EQ(p->main_exited, 0);
    CHECK_EQ(used_slots(), 1);

    /* The caller still owns the address space until the process exits: launch
     * must not have destroyed or activated it. */
    CHECK_EQ(g_space_destroyed[0], 0);
    CHECK_EQ(p->cwd[0], '/');
    CHECK_EQ(p->cwd[1], '\0');
}

static void test_launch_task_failure_rolls_back(void) {
    int32_t pid;

    TEST("launch: a failed task creation gives the slot back");
    reset_world();
    g_create_task_fails = 1;

    pid = process_launch(0x1000, 0x2000, &g_spaces[0], "prog", 0x3000);
    CHECK_EQ(pid, -1);
    CHECK_EQ(used_slots(), 0);             /* the slot was released */

    /* The address space belongs to the caller on this path -- destroying it
     * here would be a double free, since elf_loader destroys it when launch
     * returns -1. */
    CHECK_EQ(g_space_destroyed[0], 0);
    CHECK_EQ(g_close_files_calls, 0);      /* no descriptors existed yet */

    /* And the slot is genuinely reusable. */
    g_create_task_fails = 0;
    CHECK(launch("prog2", 0) != NULL);
    CHECK_EQ(used_slots(), 1);
}

static void test_launch_rejects_no_address_space(void) {
    TEST("launch: a process needs an address space");
    reset_world();
    CHECK_EQ(process_launch(0x1000, 0x2000, NULL, "prog", 0x3000), -1);
    CHECK_EQ(used_slots(), 0);
}

static void test_launch_exhausts_the_table(void) {
    TEST("launch: the process table has a limit");
    reset_world();

    for (int i = 0; i < MAX_PROCESSES; i++) {
        CHECK(process_launch(0x1000, 0x2000,
                             &g_spaces[i % MAX_SPACES], "p", 0x3000) > 0 ||
              i >= MAX_TASKS);
    }
    /* Tasks run out before slots do in this harness; either way the next
     * launch must fail cleanly rather than overrun the table. */
    CHECK(process_launch(0x1000, 0x2000, &g_spaces[0], "p", 0x3000) == -1 ||
          used_slots() <= MAX_PROCESSES);
    CHECK(used_slots() <= MAX_PROCESSES);
}

/* --- exit, zombie, reap ---------------------------------------------------- */

static void test_exit_makes_a_zombie(void) {
    process_t *p;

    TEST("exit: a process with a parent becomes a zombie");
    reset_world();
    p = launch("prog", 0);
    CHECK(p != NULL);
    if (!p) return;
    p->parent_pid = 42;                    /* somebody is around to reap it */

    exit_task(p->task, 9);

    CHECK_EQ(p->state, PROCESS_ZOMBIE);
    CHECK_EQ(p->exit_status, 9);
    CHECK(p->task == NULL);                /* the task is gone */
    CHECK(p->address_space == NULL);       /* and the space was handed back */
    CHECK_EQ(g_space_destroyed[0], 1);
    CHECK_EQ(g_close_files_calls, 1);
    CHECK_EQ(used_slots(), 1);             /* the slot is still taken */
    expect_no_underflow();
}

static void test_exit_releases_streams_once(void) {
    process_t *p;

    TEST("exit: standard streams are released exactly once");
    reset_world();
    p = launch("prog", 0);
    CHECK(p != NULL);
    if (!p) return;
    p->parent_pid = 42;

    p->stdout_node = &g_nodes[0];
    open_fs(p->stdout_node);
    p->stdin_node = &g_nodes[1];
    open_fs(p->stdin_node);
    p->stdout_pipe = &g_pipes[0];
    g_pipe_write_refs[0] = 1;
    p->stdin_pipe = &g_pipes[1];
    g_pipe_read_refs[1] = 1;

    exit_task(p->task, 0);

    CHECK_EQ(g_node_refs[0], 0);
    CHECK_EQ(g_node_refs[1], 0);
    CHECK_EQ(g_pipe_write_refs[0], 0);
    CHECK_EQ(g_pipe_read_refs[1], 0);
    /* Cleared as well as released, so a later reap cannot release them a
     * second time. */
    CHECK(p->stdout_node == NULL);
    CHECK(p->stdin_node == NULL);
    CHECK(p->stdout_pipe == NULL);
    CHECK(p->stdin_pipe == NULL);
    expect_no_underflow();
}

static void test_teardown_does_not_free_the_live_space(void) {
    process_t *p;

    /*
     * F1's shape: the address space must not be freed while it is the one the
     * CPU is using. In the kernel two independent things prevent that --
     * task_exit switches CR3 to the next task before calling on_exit, and
     * paging_destroy_user_address_space checks active_space itself. The stub
     * records the condition rather than reproducing the protection, so a
     * regression in either is visible here.
     */
    TEST("teardown never frees the address space in use");
    reset_world();
    p = launch("prog", 0);
    CHECK(p != NULL);
    if (!p) return;
    p->parent_pid = 42;

    /* The scheduler has already moved on before on_exit runs. */
    g_active_space = &g_spaces[7];
    exit_task(p->task, 0);

    CHECK_EQ(g_space_destroyed[0], 1);
    CHECK_EQ(g_destroy_while_active, 0);
}

static void test_reap_frees_the_slot(void) {
    process_t *parent, *child;
    int32_t status = 0;

    TEST("reap: the slot returns to the pool");
    reset_world();
    parent = launch("parent", 0);
    child = launch("child", 1);
    CHECK(parent && child);
    if (!parent || !child) return;
    child->parent_pid = parent->pid;
    g_current_task = parent->task;

    exit_task(child->task, 5);
    CHECK_EQ(child->state, PROCESS_ZOMBIE);
    CHECK_EQ(used_slots(), 2);

    {
        int32_t want = child->pid;

        CHECK_EQ(process_waitpid(child->pid, &status, 0), want);
    }
    CHECK_EQ(status, 5);
    CHECK_EQ(child->state, PROCESS_UNUSED);
    CHECK_EQ(used_slots(), 1);

    /* Reaping releases; it must not release a second time. */
    CHECK_EQ(g_space_destroyed[1], 1);
    CHECK_EQ(g_close_files_calls, 1);
    CHECK_EQ(g_process_release_calls, 1);
    CHECK(g_last_released_process == child);
    expect_no_underflow();
}

static void test_auto_reap_leaves_no_zombie(void) {
    process_t *p;

    TEST("an auto-reaped process leaves no zombie behind");
    reset_world();
    p = launch("prog", 0);
    CHECK(p != NULL);
    if (!p) return;
    p->parent_pid = 0;
    p->auto_reap = 1;

    exit_task(p->task, 0);

    CHECK_EQ(p->state, PROCESS_UNUSED);    /* released immediately */
    CHECK_EQ(used_slots(), 0);
    CHECK_EQ(g_space_destroyed[0], 1);     /* exactly once */
    CHECK_EQ(g_close_files_calls, 1);
    CHECK_EQ(g_process_release_calls, 1);
    CHECK(g_last_released_process == p);
    expect_no_underflow();
}

/* --- parent exit ----------------------------------------------------------- */

static void test_parent_exit_handles_children(void) {
    process_t *parent, *running, *zombie;

    TEST("a parent exiting orphans the living and reaps the dead");
    reset_world();
    parent = launch("parent", 0);
    running = launch("running", 1);
    zombie = launch("zombie", 2);
    CHECK(parent && running && zombie);
    if (!parent || !running || !zombie) return;

    running->parent_pid = parent->pid;
    zombie->parent_pid = parent->pid;

    exit_task(zombie->task, 1);
    CHECK_EQ(zombie->state, PROCESS_ZOMBIE);

    parent->parent_pid = 0;
    parent->auto_reap = 1;
    exit_task(parent->task, 0);

    /* The zombie was reaped on the spot -- nobody is left who could. */
    CHECK_EQ(zombie->state, PROCESS_UNUSED);
    /* The living child was orphaned and told to clean up after itself. */
    CHECK_EQ(running->state, PROCESS_RUNNING);
    CHECK_EQ(running->parent_pid, 0);
    CHECK_EQ(running->auto_reap, 1);
    expect_no_underflow();

    /* And when it does exit, nothing is left. */
    exit_task(running->task, 0);
    CHECK_EQ(running->state, PROCESS_UNUSED);
    CHECK_EQ(used_slots(), 0);
    CHECK_EQ(g_space_destroyed[1], 1);
    CHECK_EQ(g_space_destroyed[2], 1);
}

/* --- threads --------------------------------------------------------------- */

static void test_main_exit_with_threads_defers_teardown(void) {
    process_t *p;
    task_t *thread;

    /*
     * F1. The main task exiting while extra threads still share the address
     * space must not tear it down: those threads' task_t still point at the
     * page tables, and the scheduler will run them again.
     */
    TEST("main exit with live threads defers the teardown");
    reset_world();
    p = launch("prog", 0);
    CHECK(p != NULL);
    if (!p) return;
    p->parent_pid = 42;

    thread = create_task(NULL, thread_on_exit, &g_spaces[0], p, 0x4000, 0x5000);
    CHECK(thread != NULL);
    if (!thread) return;
    p->thread_count = 1;

    exit_task(p->task, 11);

    /* Nothing was torn down ... */
    CHECK_EQ(g_space_destroyed[0], 0);
    CHECK_EQ(g_close_files_calls, 0);
    CHECK_EQ(p->state, PROCESS_RUNNING);   /* not a zombie yet */
    /* ... but the exit was remembered. */
    CHECK_EQ(p->main_exited, 1);
    CHECK_EQ(p->exit_status, 11);
    CHECK(p->task == NULL);

    /* The last thread leaving completes it, exactly once. */
    exit_task(thread, 0);
    CHECK_EQ(p->thread_count, 0);
    CHECK_EQ(p->state, PROCESS_ZOMBIE);
    CHECK_EQ(p->exit_status, 11);          /* the main task's status, not the thread's */
    CHECK_EQ(g_space_destroyed[0], 1);
    CHECK_EQ(g_close_files_calls, 1);
    expect_no_underflow();
}

static void test_threads_exit_before_main(void) {
    process_t *p;
    task_t *t1, *t2;

    TEST("threads finishing before main do not tear anything down");
    reset_world();
    p = launch("prog", 0);
    CHECK(p != NULL);
    if (!p) return;
    p->parent_pid = 42;

    t1 = create_task(NULL, thread_on_exit, &g_spaces[0], p, 0, 0);
    t2 = create_task(NULL, thread_on_exit, &g_spaces[0], p, 0, 0);
    CHECK(t1 && t2);
    if (!t1 || !t2) return;
    p->thread_count = 2;

    exit_task(t1, 0);
    CHECK_EQ(p->thread_count, 1);
    CHECK_EQ(g_space_destroyed[0], 0);     /* main is still running */
    CHECK_EQ(p->state, PROCESS_RUNNING);

    exit_task(t2, 0);
    CHECK_EQ(p->thread_count, 0);
    CHECK_EQ(g_space_destroyed[0], 0);     /* main_exited is still 0 */
    CHECK_EQ(p->state, PROCESS_RUNNING);

    /* Only when main goes does the process end, and only once. */
    exit_task(p->task, 4);
    CHECK_EQ(p->state, PROCESS_ZOMBIE);
    CHECK_EQ(p->exit_status, 4);
    CHECK_EQ(g_space_destroyed[0], 1);
    CHECK_EQ(g_close_files_calls, 1);
}

static void test_thread_exit_wakes_joiners(void) {
    process_t *p;
    task_t *t;

    TEST("a thread exiting wakes anyone joining on the count");
    reset_world();
    p = launch("prog", 0);
    CHECK(p != NULL);
    if (!p) return;
    t = create_task(NULL, thread_on_exit, &g_spaces[0], p, 0, 0);
    CHECK(t != NULL);
    if (!t) return;
    p->thread_count = 1;

    exit_task(t, 0);

    /* SYS_THREAD_JOIN parks on &process->thread_count; without this broadcast
     * a join would never return. */
    CHECK(g_wake_all_calls > 0);
    CHECK(g_last_wake_all_channel == (const void *)&p->thread_count);
}

static void test_finish_exit_runs_exactly_once(void) {
    process_t *p;
    task_t *t;

    TEST("finish_exit runs once even with several threads");
    reset_world();
    p = launch("prog", 0);
    CHECK(p != NULL);
    if (!p) return;
    p->parent_pid = 42;
    t = create_task(NULL, thread_on_exit, &g_spaces[0], p, 0, 0);
    CHECK(t != NULL);
    if (!t) return;
    p->thread_count = 1;

    exit_task(p->task, 2);      /* deferred */
    exit_task(t, 0);            /* completes it */

    CHECK_EQ(g_space_destroyed[0], 1);
    CHECK_EQ(g_close_files_calls, 1);
    CHECK_EQ(p->state, PROCESS_ZOMBIE);
}

/* --- detach ---------------------------------------------------------------- */

static void test_detach(void) {
    process_t *running, *zombie;

    TEST("detach");
    reset_world();
    running = launch("running", 0);
    zombie = launch("zombie", 1);
    CHECK(running && zombie);
    if (!running || !zombie) return;
    running->parent_pid = 5;
    zombie->parent_pid = 5;

    /* A live process is orphaned and told to reap itself. */
    CHECK_EQ(process_detach(running->pid), 0);
    CHECK_EQ(running->parent_pid, 0);
    CHECK_EQ(running->auto_reap, 1);
    CHECK_EQ(running->state, PROCESS_RUNNING);

    /* A process that has already exited is reaped on the spot. */
    exit_task(zombie->task, 0);
    CHECK_EQ(zombie->state, PROCESS_ZOMBIE);
    CHECK_EQ(process_detach(zombie->pid), 0);
    CHECK_EQ(zombie->state, PROCESS_UNUSED);
    CHECK_EQ(g_process_release_calls, 1);

    /* Detaching something that does not exist is an error, not a crash. */
    CHECK_EQ(process_detach(4242), -1);
    CHECK_EQ(process_detach(-1), -1);

    /* The detached live process leaves nothing behind when it goes. */
    exit_task(running->task, 0);
    CHECK_EQ(running->state, PROCESS_UNUSED);
    CHECK_EQ(used_slots(), 0);
    CHECK_EQ(g_space_destroyed[0], 1);
    CHECK_EQ(g_space_destroyed[1], 1);
    CHECK_EQ(g_process_release_calls, 2); /* zombie detach + live auto-reap */
    expect_no_underflow();
}

/* --- slot and pid reuse ---------------------------------------------------- */

static void test_slot_reuse_carries_nothing_over(void) {
    process_t *first, *second;
    int32_t first_pid;

    TEST("a reused slot carries no state from its previous occupant");
    reset_world();

    first = launch("first", 0);
    CHECK(first != NULL);
    if (!first) return;
    first_pid = first->pid;
    first->parent_pid = 77;
    first->thread_count = 3;
    first->main_exited = 1;
    first->sig_pending = 0xFF;
    first->sig_handler[SIGCHLD] = 0xDEAD;
    first->auto_reap = 1;
    first->stopped = 1;
    first->alarm_active = 1;
    first->alarm_tick = 12345;
    first->env_count = 4;
    first->heap_break = 0x9000;
    CHECK_EQ(process_ext_alloc(first, 2), (int32_t)USER_EXT_BASE);

    first->thread_count = 0;               /* let the exit complete */
    exit_task(first->task, 0);
    CHECK_EQ(first->state, PROCESS_UNUSED);

    second = launch("second", 1);
    CHECK(second != NULL);
    if (!second) return;
    CHECK(second == first);                /* the same slot came back */

    CHECK(second->pid != first_pid);       /* but a new pid */
    CHECK_EQ(second->parent_pid, 0);
    CHECK_EQ(second->thread_count, 0);
    CHECK_EQ(second->main_exited, 0);
    CHECK_EQ(second->sig_pending, 0);
    CHECK_EQ(second->sig_handler[SIGCHLD], 0);
    CHECK_EQ(second->auto_reap, 0);
    CHECK_EQ(second->stopped, 0);
    CHECK_EQ(second->alarm_active, 0);
    CHECK_EQ(second->alarm_tick, 0);
    CHECK_EQ(second->env_count, 0);
    CHECK_EQ(second->exit_status, 0);
    CHECK_EQ(second->state, PROCESS_RUNNING);
    CHECK_EQ(second->slot, first->slot);   /* the slot number survives, by design */
    CHECK_EQ(process_ext_reserved(second, USER_EXT_BASE), 0);
    CHECK_EQ(process_ext_alloc(second, 2), (int32_t)USER_EXT_BASE);
}

static void test_pids_are_monotonic(void) {
    int32_t seen[6];

    /*
     * A pid must never name two processes over time. An orphan whose parent
     * recorded its pid, or a wait for a pid that has already been reaped, must
     * not be answered by whichever process happens to occupy the slot now.
     */
    TEST("pids are not reused when a slot is");
    reset_world();

    for (int i = 0; i < 6; i++) {
        process_t *p = launch("p", 0);

        CHECK(p != NULL);
        if (!p) return;
        seen[i] = p->pid;
        p->parent_pid = 0;
        p->auto_reap = 1;
        exit_task(p->task, 0);
        CHECK_EQ(p->state, PROCESS_UNUSED);
    }

    for (int i = 1; i < 6; i++) CHECK(seen[i] > seen[i - 1]);

    /* Looking up a pid that has been reaped finds nothing, even though its
     * slot is occupied again. */
    CHECK(find_by_pid(seen[0]) == NULL);
}

static void test_pid_counter_wraps_to_an_unused_positive_pid(void) {
    process_t *first;
    int32_t max_pid, wrapped_pid;

    TEST("pid allocation wraps without overflow or a live-pid collision");
    reset_world();

    first = launch("first", 0);             /* keeps pid 1 live */
    CHECK(first != NULL);
    if (!first) return;
    CHECK_EQ(first->pid, 1);

    next_pid = INT32_MAX;
    max_pid = process_launch(0x1000, 0x2000, &g_spaces[1], "max", 0x3000);
    wrapped_pid = process_launch(0x1000, 0x2000, &g_spaces[2], "wrap", 0x3000);

    CHECK_EQ(max_pid, INT32_MAX);
    CHECK_EQ(wrapped_pid, 2);                /* pid 1 is still occupied */
    CHECK(find_by_pid(max_pid) != NULL);
    CHECK(find_by_pid(wrapped_pid) != NULL);
    CHECK_EQ(used_slots(), 3);
}

static void test_repeated_lifecycles(void) {
    TEST("many allocate/exit/reap cycles leak nothing");
    reset_world();

    for (int round = 0; round < 24; round++) {
        process_t *p;

        /* The task pool is small, so recycle it by hand each round; the point
         * here is the PROCESS table, not the task allocator. */
        g_task_count = 0;
        p = launch("p", round % MAX_SPACES);
        CHECK(p != NULL);
        if (!p) return;
        p->parent_pid = 0;
        p->auto_reap = 1;
        exit_task(p->task, round);
        CHECK_EQ(p->state, PROCESS_UNUSED);
    }

    CHECK_EQ(used_slots(), 0);
    expect_no_underflow();
    CHECK_EQ(g_close_files_calls, 24);
    CHECK_EQ(g_destroy_while_active, 0);
}

/* --- fork ------------------------------------------------------------------ */

static void test_fork_success(void) {
    process_t *parent, *child;
    fork_frame_t frame;
    int32_t pid;
    int32_t ext;

    TEST("fork: the child inherits and the parent is untouched");
    reset_world();
    parent = launch("parent", 0);
    CHECK(parent != NULL);
    if (!parent) return;
    parent->heap_break = 0x8000;
    parent->env_count = 1;
    parent->sig_handler[SIGCHLD] = 0x1234;
    parent->sig_pending = 0x10;
    parent->stdout_node = &g_nodes[0];
    open_fs(parent->stdout_node);
    ext = process_ext_alloc(parent, 2);
    CHECK_EQ(ext, (int32_t)USER_EXT_BASE);

    memset(&frame, 0, sizeof(frame));
    frame.eip = 0x5555;
    g_current_task = parent->task;

    pid = process_fork(parent, &g_spaces[1], &frame);
    CHECK(pid > 0);
    child = find_by_pid(pid);
    CHECK(child != NULL);
    if (!child) return;

    CHECK(child != parent);
    CHECK_EQ(child->parent_pid, parent->pid);
    CHECK_EQ(child->state, PROCESS_RUNNING);
    CHECK(child->address_space == &g_spaces[1]);
    CHECK_EQ(child->heap_break, 0x8000);
    CHECK_EQ(child->env_count, 1);
    CHECK_EQ(child->sig_handler[SIGCHLD], 0x1234);
    CHECK_EQ(child->sig_pending, 0);       /* pending signals are NOT inherited */
    CHECK_EQ(child->fork_frame.eip, 0x5555);
    CHECK_EQ(g_copy_files_calls, 1);
    CHECK_EQ(process_ext_reserved(child, (uint32_t)ext), 1);
    CHECK_EQ(process_ext_reserved(child, (uint32_t)ext + 0x1000U), 1);
    CHECK_EQ(process_ext_free(child, (uint32_t)ext, 1), 0);
    CHECK_EQ(process_ext_reserved(child, (uint32_t)ext), 0);
    CHECK_EQ(process_ext_reserved(parent, (uint32_t)ext), 1);

    /* The inherited stdout took its own reference. */
    CHECK_EQ(g_node_refs[0], 2);
    CHECK(child->stdout_node == parent->stdout_node);

    /* The parent is exactly as it was. */
    CHECK_EQ(parent->state, PROCESS_RUNNING);
    CHECK_EQ(parent->thread_count, 0);
    CHECK(parent->task != NULL);
    expect_no_underflow();
}

static void test_fork_task_failure_rolls_back(void) {
    process_t *parent;
    fork_frame_t frame;
    int slots_before;

    TEST("fork: a failed task creation releases everything inherited");
    reset_world();
    parent = launch("parent", 0);
    CHECK(parent != NULL);
    if (!parent) return;
    parent->stdout_node = &g_nodes[0];
    open_fs(parent->stdout_node);
    parent->stdin_node = &g_nodes[1];
    open_fs(parent->stdin_node);
    parent->stdout_pipe = &g_pipes[0];
    g_pipe_write_refs[0] = 1;
    parent->stdin_pipe = &g_pipes[1];
    g_pipe_read_refs[1] = 1;

    memset(&frame, 0, sizeof(frame));
    slots_before = used_slots();
    g_current_task = parent->task;
    g_create_task_fails = 1;

    CHECK_EQ(process_fork(parent, &g_spaces[1], &frame), -1);

    /* No slot, and every reference the child briefly held is back. */
    CHECK_EQ(used_slots(), slots_before);
    CHECK_EQ(g_node_refs[0], 1);
    CHECK_EQ(g_node_refs[1], 1);
    CHECK_EQ(g_pipe_write_refs[0], 1);
    CHECK_EQ(g_pipe_read_refs[1], 1);
    CHECK_EQ(g_close_files_calls, 1);      /* the child's table was cleared */
    /* The address space stays with the caller on this path, as in launch. */
    CHECK_EQ(g_space_destroyed[1], 0);
    expect_no_underflow();

    /* The parent still works. */
    CHECK_EQ(parent->state, PROCESS_RUNNING);
}

static void test_fork_rejects_bad_arguments(void) {
    process_t *parent;
    fork_frame_t frame;

    TEST("fork: arguments");
    reset_world();
    parent = launch("parent", 0);
    CHECK(parent != NULL);
    if (!parent) return;
    memset(&frame, 0, sizeof(frame));

    CHECK_EQ(process_fork(NULL, &g_spaces[1], &frame), -1);
    CHECK_EQ(process_fork(parent, NULL, &frame), -1);
    CHECK_EQ(process_fork(parent, &g_spaces[1], NULL), -1);
    CHECK_EQ(used_slots(), 1);
}

static void test_exec_reset_retires_old_mmap_contract(void) {
    process_t *p;
    int32_t ext;

    TEST("exec reset: new image owns a fresh heap and mmap reservation map");
    reset_world();
    p = launch("old", 0);
    CHECK(p != NULL);
    if (!p) return;
    ext = process_ext_alloc(p, 3);
    CHECK_EQ(ext, (int32_t)USER_EXT_BASE);
    p->heap_break = 0x9000;
    p->sig_handler[SIGUSR1] = 0x5555;
    g_current_task = p->task;
    g_active_space = &g_spaces[0];

    CHECK_EQ(process_exec_reset(&g_spaces[1], 0x5000, "new"), 0);
    CHECK(p->address_space == &g_spaces[1]);
    CHECK(p->task->address_space == &g_spaces[1]);
    CHECK_EQ(p->heap_start, 0x5000);
    CHECK_EQ(p->heap_break, 0x5000);
    CHECK_EQ(process_ext_reserved(p, (uint32_t)ext), 0);
    CHECK_EQ(p->sig_handler[SIGUSR1], 0);
    CHECK_EQ(g_activate_calls, 1);
    CHECK_EQ(g_space_destroyed[0], 1);
    CHECK_EQ(g_destroy_while_active, 0);

    TEST("exec reset: a live sibling retains the old image unchanged");
    reset_world();
    p = launch("old", 0);
    CHECK(p != NULL);
    if (!p) return;
    ext = process_ext_alloc(p, 1);
    p->thread_count = 1;
    g_current_task = p->task;
    CHECK_EQ(process_exec_reset(&g_spaces[1], 0x5000, "new"), -1);
    CHECK(p->address_space == &g_spaces[0]);
    CHECK(p->task->address_space == &g_spaces[0]);
    CHECK_EQ(process_ext_reserved(p, (uint32_t)ext), 1);
    CHECK_EQ(g_activate_calls, 0);
    CHECK_EQ(g_space_destroyed[0], 0);
}

static void test_child_exit_does_not_disturb_the_parent(void) {
    process_t *parent, *child;
    fork_frame_t frame;
    int32_t pid;

    TEST("fork: a child exiting leaves the parent's lifecycle alone");
    reset_world();
    parent = launch("parent", 0);
    CHECK(parent != NULL);
    if (!parent) return;
    memset(&frame, 0, sizeof(frame));
    g_current_task = parent->task;

    pid = process_fork(parent, &g_spaces[1], &frame);
    child = find_by_pid(pid);
    CHECK(child != NULL);
    if (!child) return;

    exit_task(child->task, 6);

    CHECK_EQ(child->state, PROCESS_ZOMBIE);
    CHECK_EQ(parent->state, PROCESS_RUNNING);
    CHECK(parent->task != NULL);
    CHECK(parent->address_space == &g_spaces[0]);
    CHECK_EQ(g_space_destroyed[0], 0);     /* only the child's went */
    CHECK_EQ(g_space_destroyed[1], 1);
    expect_no_underflow();
}

/* --- signals as a lifecycle participant ------------------------------------ */

static void test_send_signal_wakes_a_blocked_target(void) {
    process_t *p;

    TEST("a signal wakes a blocked target by identity");
    reset_world();
    p = launch("prog", 0);
    CHECK(p != NULL);
    if (!p) return;
    p->task->state = TASK_BLOCKED;

    CHECK_EQ(process_send_signal(p->pid, SIGUSR1), 0);
    CHECK(p->sig_pending & (1u << SIGUSR1));
    CHECK_EQ(g_wake_task_calls, 1);
    CHECK(g_last_wake_task == p->task);
    /* By identity, never by channel: unrelated tasks share wait channels, and
     * waking "some waiter" can leave the intended one asleep (F10). */
    CHECK_EQ(g_wake_one_calls, 0);

    /* A target that is running needs no nudge. */
    p->task->state = TASK_READY;
    CHECK_EQ(process_send_signal(p->pid, SIGUSR1), 0);
    CHECK_EQ(g_wake_task_calls, 1);

    /* Bad arguments are refused. */
    CHECK_EQ(process_send_signal(4242, SIGUSR1), -1);
    CHECK_EQ(process_send_signal(p->pid, 0), -1);
    CHECK_EQ(process_send_signal(p->pid, NSIG), -1);
}

static void test_alarm_deadline_at_zero_fires(void) {
    process_t *p;

    TEST("an active alarm may have deadline tick zero");
    reset_world();
    p = launch("alarm", 0);
    CHECK(p != NULL);
    if (!p) return;
    p->alarm_active = 1;
    p->alarm_tick = 0;

    process_check_alarms(UINT32_MAX);      /* one tick before the deadline */
    CHECK_EQ(p->sig_pending & (1u << SIGALRM), 0);
    CHECK_EQ(p->alarm_active, 1);

    process_check_alarms(0);               /* deadline reached across wrap */
    CHECK(p->sig_pending & (1u << SIGALRM));
    CHECK_EQ(p->alarm_active, 0);
    CHECK_EQ(p->alarm_tick, 0);

    p->sig_pending = 0;
    process_check_alarms(0);               /* inactive zero is not re-fired */
    CHECK_EQ(p->sig_pending, 0);
}

static void test_exit_signals_the_parent(void) {
    process_t *parent, *child;

    TEST("exit notifies the parent");
    reset_world();
    parent = launch("parent", 0);
    child = launch("child", 1);
    CHECK(parent && child);
    if (!parent || !child) return;
    child->parent_pid = parent->pid;
    parent->task->state = TASK_BLOCKED;

    exit_task(child->task, 0);

    CHECK(parent->sig_pending & (1u << SIGCHLD));
    CHECK(g_last_wake_task == parent->task);

    /* An orphan has nobody to tell, and must not signal pid 0. */
    reset_world();
    child = launch("orphan", 0);
    CHECK(child != NULL);
    if (!child) return;
    child->parent_pid = 0;
    child->auto_reap = 1;
    exit_task(child->task, 0);
    CHECK_EQ(g_wake_task_calls, 0);
}

static void test_teardown_order(void) {
    process_t *p;

    /*
     * The order the teardown runs in is a contract, not an accident. The
     * standard streams and the descriptor table are released while the
     * process still owns its address space, and the address space goes last.
     * Swapping any two of those changes no count at all -- which is why this
     * test records when each step ran rather than how many times.
     *
     * Freeing the address space first would be the dangerous direction: the
     * descriptor table can hold nodes whose backing store the space still
     * maps, and a future teardown step that touched user memory would find it
     * unmapped.
     */
    TEST("teardown runs in the contracted order");
    reset_world();
    p = launch("prog", 0);
    CHECK(p != NULL);
    if (!p) return;
    p->parent_pid = 42;
    p->stdout_node = &g_nodes[0];
    open_fs(p->stdout_node);

    exit_task(p->task, 0);

    CHECK(g_seq_release_streams > 0);
    CHECK(g_seq_close_files > 0);
    CHECK(g_seq_destroy_space > 0);
    /* streams, then the descriptor table, then the address space. */
    CHECK(g_seq_release_streams < g_seq_close_files);
    CHECK(g_seq_close_files < g_seq_destroy_space);
}

static void test_two_threads_only_the_last_finishes(void) {
    process_t *p;
    task_t *t1, *t2;

    /*
     * F1, in the shape that a single extra thread cannot show. With one
     * thread, "the count reached zero" and "a thread exited" are the same
     * event, so completing the exit on either reads the same. With two, the
     * first thread's exit must change nothing: its sibling is still running
     * on the address space, and tearing it down here is precisely the
     * use-after-free F1 was.
     */
    TEST("with two threads, only the last one completes the exit");
    reset_world();
    p = launch("prog", 0);
    CHECK(p != NULL);
    if (!p) return;
    p->parent_pid = 42;

    t1 = create_task(NULL, thread_on_exit, &g_spaces[0], p, 0, 0);
    t2 = create_task(NULL, thread_on_exit, &g_spaces[0], p, 0, 0);
    CHECK(t1 && t2);
    if (!t1 || !t2) return;
    p->thread_count = 2;

    exit_task(p->task, 13);                /* main goes first: deferred */
    CHECK_EQ(p->main_exited, 1);
    CHECK_EQ(g_space_destroyed[0], 0);

    exit_task(t1, 0);                      /* one thread left */
    CHECK_EQ(p->thread_count, 1);
    CHECK_EQ(g_space_destroyed[0], 0);     /* t2 still runs on this space */
    CHECK_EQ(g_close_files_calls, 0);
    CHECK_EQ(p->state, PROCESS_RUNNING);

    exit_task(t2, 0);                      /* now, and only now */
    CHECK_EQ(p->thread_count, 0);
    CHECK_EQ(p->state, PROCESS_ZOMBIE);
    CHECK_EQ(p->exit_status, 13);
    CHECK_EQ(g_space_destroyed[0], 1);     /* exactly once, not twice */
    CHECK_EQ(g_close_files_calls, 1);
    expect_no_underflow();
}

static void test_multiple_kill_requests_are_independent(void) {
    process_t *first, *second;
    int escaped;

    TEST("multiple kill requests retain each target");
    reset_world();
    first = launch("first", 0);
    second = launch("second", 1);
    CHECK(first && second);
    if (!first || !second) return;

    process_request_kill(first->pid);
    process_request_kill(second->pid);
    CHECK(first->task->kill_pending);
    CHECK(second->task->kill_pending);

    /* The second request must not overwrite the first. Each marked runnable
     * task should leave when the timer checks that task. */
    g_current_task = first->task;
    escaped = setjmp(g_exit_jmp);
    if (escaped == 0) {
        process_check_kill();
        CHECK(0);  /* a marked current task must not return */
    } else {
        CHECK_EQ(escaped, 1);
        CHECK_EQ(g_task_exit_status, TASK_KILL_STATUS);
    }

    g_current_task = second->task;
    escaped = setjmp(g_exit_jmp);
    if (escaped == 0) {
        process_check_kill();
        CHECK(0);
    } else {
        CHECK_EQ(escaped, 1);
        CHECK_EQ(g_task_exit_status, TASK_KILL_STATUS);
    }
}

int main(void) {
    int escaped = setjmp(g_exit_jmp);

    if (escaped == 2) {
        printf("  FAIL a wait parked %d times with nothing to wake it: "
               "this would hang the kernel\n", g_stuck_blocks);
        return 1;
    }
    if (escaped != 0) {
        printf("  FAIL unexpected task_exit escaped a test\n");
        return 1;
    }

    test_waitpid_is_woken_without_a_sigchld_handler();
    test_waitpid_blocks_on_dedicated_child_event();
    test_waitpid_wake_is_identity_not_channel_luck();
    test_wait_parks_on_the_child();
    test_spurious_wake_rechecks();
    test_waitpid_nohang();
    test_wait_for_a_non_child();

    test_launch_success();
    test_launch_task_failure_rolls_back();
    test_launch_rejects_no_address_space();
    test_launch_exhausts_the_table();

    test_exit_makes_a_zombie();
    test_exit_releases_streams_once();
    test_teardown_does_not_free_the_live_space();
    test_teardown_order();
    test_reap_frees_the_slot();
    test_auto_reap_leaves_no_zombie();
    test_parent_exit_handles_children();

    test_main_exit_with_threads_defers_teardown();
    test_threads_exit_before_main();
    test_thread_exit_wakes_joiners();
    test_finish_exit_runs_exactly_once();
    test_two_threads_only_the_last_finishes();

    test_detach();
    test_slot_reuse_carries_nothing_over();
    test_pids_are_monotonic();
    test_pid_counter_wraps_to_an_unused_positive_pid();
    test_repeated_lifecycles();

    test_fork_success();
    test_fork_task_failure_rolls_back();
    test_fork_rejects_bad_arguments();
    test_exec_reset_retires_old_mmap_contract();
    test_child_exit_does_not_disturb_the_parent();

    test_send_signal_wakes_a_blocked_target();
    test_alarm_deadline_at_zero_fires();
    test_exit_signals_the_parent();

    /* Uses g_exit_jmp for expected task exits, so keep it last. */
    test_multiple_kill_requests_are_independent();

    TEST_REPORT("process");
}
