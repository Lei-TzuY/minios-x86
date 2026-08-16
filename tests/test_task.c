#include "test.h"
#include "../task.h"
#include "../pmm.h"

#include <setjmp.h>
#include <stddef.h>

/*
 * The scheduler's ready ring and blocked list are pure pointer logic, and the
 * kind of thing where an off-by-one or a wrong link update corrupts the run
 * queue in a way the shell only shows as a much later hang. The context switch
 * itself is assembly (switch_task) and is stubbed to a no-op here, which leaves
 * the list operations fully exercisable:
 *
 *   - task_block_current moves the current task out of the ready ring and
 *     APPENDS it to the blocked list (the FIFO ordering fixed in FAIR1),
 *   - task_wake_one / task_wake_all wake by wait channel,
 *   - task_wake_task wakes a SPECIFIC task by identity (added in F10 to fix a
 *     deadlock); until now it was only covered indirectly by the job-control
 *     end-to-end test,
 *   - task_kill_blocked flags every task of one process and wakes the blocked
 *     ones, which is what lets a kill reach a task parked in a wait (F19).
 *
 * All of that runs without a real context switch, so it can be checked here
 * directly.
 */

/* --- stubs for everything task.c touches except the list logic ------------ */
static uint8_t pool[80][PMM_BLOCK_SIZE] __attribute__((aligned(PMM_BLOCK_SIZE)));
static int pool_next;
static void *freed_blocks[8];
static int free_count;
static jmp_buf exit_switch_jmp;
static int intercept_exit_switch;

void *pmm_alloc_block(void) {
    return pool_next < (int)(sizeof(pool) / sizeof(pool[0])) ? pool[pool_next++] : NULL;
}
void pmm_free_block(void *p) {
    if (free_count < (int)(sizeof(freed_blocks) / sizeof(freed_blocks[0]))) {
        freed_blocks[free_count] = p;
    }
    free_count++;
}

void switch_task(uint32_t *old_esp, uint32_t new_esp) {
    (void)old_esp;
    (void)new_esp;
    if (intercept_exit_switch) longjmp(exit_switch_jmp, 1);
}
void set_kernel_stack(uint32_t s) { (void)s; }
void paging_activate_address_space(address_space_t *s) { (void)s; }
address_space_t *paging_get_kernel_address_space(void) {
    static int dummy; return (address_space_t *)&dummy;
}
void terminal_writestring(const char *s) { (void)s; }

/* task.c globals we read to inspect the ring. */
extern volatile task_t *current_task;
extern volatile task_t *ready_queue;

static void noop_entry(void) {}

/* --- helpers to inspect the ready ring ------------------------------------ */
static int ready_count(void) {
    task_t *t, *head;
    int n = 0;
    if (!ready_queue) return 0;
    head = (task_t *)ready_queue;
    t = head;
    do { n++; t = t->next; } while (t != head && n < 1000);
    return n;
}
static int ready_contains(task_t *x) {
    task_t *t, *head;
    if (!ready_queue) return 0;
    head = (task_t *)ready_queue;
    t = head;
    do { if (t == x) return 1; t = t->next; } while (t != head);
    return 0;
}

/* Block a chosen task on a channel, standing in for it being the running task
 * that decides to wait. */
static void block_task(task_t *t, const void *chan) {
    current_task = t;
    task_block_current(chan);
}

static int ch1, ch2;   /* two distinct wait channels (compared by address) */

static void test_setup(void) {
    TEST("tasking_init builds the ring");
    tasking_init();                       /* main task + idle task */
    CHECK_EQ(ready_count(), 2);
    CHECK_EQ(task_get_blocked_count(), 0);
    CHECK(current_task != NULL);
}

static void test_block_removes_from_ready(void) {
    task_t *a = create_task(noop_entry, NULL, paging_get_kernel_address_space(),
                            NULL, 0, 0);
    TEST("block moves a task out of the ready ring");
    CHECK(a != NULL);
    CHECK_EQ(ready_count(), 3);           /* main, idle, a */
    CHECK(ready_contains(a));

    block_task(a, &ch1);
    CHECK(!ready_contains(a));
    CHECK_EQ(ready_count(), 2);
    CHECK_EQ(task_get_blocked_count(), 1);
    CHECK_EQ(a->state, TASK_BLOCKED);     /* state tracks the transition */
    CHECK(a->wait_channel == &ch1);

    /* Wake it back and the ring grows again. A woken task must be READY again,
     * with its wait channel cleared -- otherwise a later block would early-out
     * on the "not READY" guard. */
    task_wake_one(&ch1);
    CHECK(ready_contains(a));
    CHECK_EQ(ready_count(), 3);
    CHECK_EQ(task_get_blocked_count(), 0);
    CHECK_EQ(a->state, TASK_READY);
    CHECK(a->wait_channel == NULL);
}

static void test_fifo_wake_order(void) {
    task_t *a = create_task(noop_entry, NULL, paging_get_kernel_address_space(), NULL, 0, 0);
    task_t *b = create_task(noop_entry, NULL, paging_get_kernel_address_space(), NULL, 0, 0);
    task_t *c = create_task(noop_entry, NULL, paging_get_kernel_address_space(), NULL, 0, 0);

    TEST("blocked list is FIFO");
    CHECK(a && b && c);
    block_task(a, &ch1);
    block_task(b, &ch1);
    block_task(c, &ch1);
    CHECK_EQ(task_get_blocked_count(), 3);

    /* task_block_current appends at the tail, and task_wake_one takes the head,
     * so the oldest waiter (a) must wake first -- head insertion would wake c. */
    task_wake_one(&ch1);
    CHECK(ready_contains(a));
    CHECK(!ready_contains(b));
    CHECK(!ready_contains(c));
    CHECK_EQ(task_get_blocked_count(), 2);

    task_wake_one(&ch1);
    CHECK(ready_contains(b));             /* then b */
    CHECK(!ready_contains(c));
    CHECK_EQ(task_get_blocked_count(), 1);

    task_wake_one(&ch1);
    CHECK(ready_contains(c));             /* finally c */
    CHECK_EQ(task_get_blocked_count(), 0);

    /* Waking an empty channel is a no-op, not a crash. */
    task_wake_one(&ch1);
    CHECK_EQ(task_get_blocked_count(), 0);
}

static void test_wake_task_by_identity(void) {
    task_t *a = create_task(noop_entry, NULL, paging_get_kernel_address_space(), NULL, 0, 0);
    task_t *b = create_task(noop_entry, NULL, paging_get_kernel_address_space(), NULL, 0, 0);
    task_t *c = create_task(noop_entry, NULL, paging_get_kernel_address_space(), NULL, 0, 0);

    /* All three on the SAME channel: this is exactly the case that broke before
     * F10 -- task_wake_one would wake an arbitrary one, but the kernel needed a
     * specific task woken. task_wake_task must pick by identity regardless of
     * position in the list. */
    TEST("wake by identity from the middle");
    CHECK(a && b && c);
    block_task(a, &ch1);
    block_task(b, &ch1);
    block_task(c, &ch1);
    CHECK_EQ(task_get_blocked_count(), 3);

    task_wake_task(b);                    /* the middle one */
    CHECK(ready_contains(b));
    CHECK(!ready_contains(a));
    CHECK(!ready_contains(c));
    CHECK_EQ(task_get_blocked_count(), 2);

    task_wake_task(c);                    /* the tail */
    CHECK(ready_contains(c));
    CHECK(!ready_contains(a));
    CHECK_EQ(task_get_blocked_count(), 1);

    TEST("wake by identity: not-blocked and NULL are no-ops");
    task_wake_task(b);                    /* already awake */
    CHECK_EQ(task_get_blocked_count(), 1);
    task_wake_task(NULL);                 /* NULL */
    CHECK_EQ(task_get_blocked_count(), 1);

    task_wake_task(a);                    /* the last one, at the head */
    CHECK(ready_contains(a));
    CHECK_EQ(task_get_blocked_count(), 0);
}

static void test_wake_channel_selectivity(void) {
    task_t *a = create_task(noop_entry, NULL, paging_get_kernel_address_space(), NULL, 0, 0);
    task_t *b = create_task(noop_entry, NULL, paging_get_kernel_address_space(), NULL, 0, 0);
    task_t *c = create_task(noop_entry, NULL, paging_get_kernel_address_space(), NULL, 0, 0);

    TEST("wake_one wakes only its channel");
    CHECK(a && b && c);
    block_task(a, &ch1);
    block_task(b, &ch2);
    block_task(c, &ch1);
    CHECK_EQ(task_get_blocked_count(), 3);

    task_wake_one(&ch2);                  /* only b waits on ch2 */
    CHECK(ready_contains(b));
    CHECK(!ready_contains(a));
    CHECK(!ready_contains(c));
    CHECK_EQ(task_get_blocked_count(), 2);

    TEST("wake_all wakes every waiter on a channel, and only those");
    task_wake_all(&ch1);                  /* a and c, not the (already awake) b */
    CHECK(ready_contains(a));
    CHECK(ready_contains(c));
    CHECK_EQ(task_get_blocked_count(), 0);

    task_wake_all(&ch1);                  /* nothing left: no-op */
    CHECK_EQ(task_get_blocked_count(), 0);
}

/* task.c only ever compares task->process by address, so any distinct
 * addresses stand in for real processes here. */
static int proc_a, proc_b;

static task_t *make_task(void *owner) {
    task_t *t = create_task(noop_entry, NULL, paging_get_kernel_address_space(),
                            (struct process *)owner, 0, 0);
    return t;
}

static void test_kill_blocked(void) {
    task_t *a = make_task(&proc_a);       /* target process, will be blocked */
    task_t *b = make_task(&proc_a);       /* target process, stays ready */
    task_t *c = make_task(&proc_b);       /* a different process, blocked */

    TEST("kill_blocked wakes the target's blocked tasks and flags them");
    CHECK(a && b && c);
    block_task(a, &ch1);
    block_task(c, &ch1);                  /* same channel, different process */
    CHECK_EQ(task_get_blocked_count(), 2);

    /* Only the blocked task of the target counts as woken; b was already
     * runnable and c belongs to someone else. */
    CHECK_EQ(task_kill_blocked((struct process *)&proc_a), 1);
    CHECK(ready_contains(a));
    CHECK_EQ(a->state, TASK_READY);       /* fully requeued, not just unlinked */
    CHECK(a->wait_channel == NULL);
    CHECK_EQ(a->kill_pending, 1);
    CHECK_EQ(task_get_blocked_count(), 1);

    TEST("kill_blocked flags the target's runnable tasks too");
    /* A ready task of the target could otherwise park in a wait before any
     * timer tick lands on it, and would then be unreachable. */
    CHECK_EQ(b->kill_pending, 1);

    TEST("kill_blocked leaves other processes alone");
    CHECK(!ready_contains(c));            /* still blocked */
    CHECK_EQ(c->state, TASK_BLOCKED);
    CHECK_EQ(c->kill_pending, 0);

    TEST("kill_blocked with nothing to do");
    CHECK_EQ(task_kill_blocked((struct process *)&proc_a), 0);  /* none left blocked */
    CHECK_EQ(task_kill_blocked(NULL), 0);                       /* NULL is a no-op */
    CHECK_EQ(c->kill_pending, 0);
    CHECK_EQ(task_get_blocked_count(), 1);

    /* Clean up: c is still blocked and would skew later counts. */
    task_wake_task(c);
    CHECK_EQ(task_get_blocked_count(), 0);
}

static void test_kill_pending_is_per_task(void) {
    task_t *a = make_task(&proc_a);       /* flagged below */
    task_t *b = make_task(&proc_b);       /* never flagged */

    /* task_block_killable() asks task_kill_pending() about the CURRENT task, so
     * a flag on one task must not make another task exit. */
    TEST("kill_pending reports the current task only");
    CHECK(a && b);
    block_task(a, &ch1);
    CHECK_EQ(task_kill_blocked((struct process *)&proc_a), 1);

    current_task = a;
    CHECK_EQ(task_kill_pending(), 1);
    current_task = b;
    CHECK_EQ(task_kill_pending(), 0);

    a->kill_pending = 0;                  /* leave the ring clean for later tests */
}

static int exit_callback_calls;
static task_t *exit_callback_task;
static int32_t exit_callback_status;

static void observe_task_exit(task_t *task, int32_t status) {
    exit_callback_calls++;
    exit_callback_task = task;
    exit_callback_status = status;
    /* task_exit switched away before this callback; its stack must still
     * exist until a later scheduler entry reaches the safe reap point. */
    CHECK_EQ(free_count, 0);
}

static void test_task_exit_defers_its_own_reclamation(void) {
    task_t *victim = create_task(noop_entry, observe_task_exit,
                                 paging_get_kernel_address_space(), NULL, 0, 0);
    void *stack;

    TEST("task_exit retires, then the next scheduler entry reaps its stack");
    CHECK(victim != NULL);
    if (!victim) return;
    stack = victim->stack_bottom;
    free_count = 0;
    exit_callback_calls = 0;
    exit_callback_task = NULL;
    exit_callback_status = 0;

    current_task = victim;
    if (setjmp(exit_switch_jmp) == 0) {
        intercept_exit_switch = 1;
        task_exit(73);
    }
    intercept_exit_switch = 0;

    CHECK(current_task != victim);         /* scheduler left its old stack */
    CHECK_EQ(exit_callback_calls, 1);
    CHECK(exit_callback_task == victim);
    CHECK_EQ(exit_callback_status, 73);
    CHECK_EQ(free_count, 0);               /* no self-stack UAF */

    /* schedule() is the later safe point. It first reaps the retired task,
     * then switches a still-live task; stack and task are released once. */
    schedule();
    CHECK_EQ(free_count, 2);
    CHECK(freed_blocks[0] == stack);
    CHECK(freed_blocks[1] == victim);
}

int main(void) {
    test_setup();
    test_block_removes_from_ready();
    test_fifo_wake_order();
    test_wake_task_by_identity();
    test_wake_channel_selectivity();
    test_kill_blocked();
    test_kill_pending_is_per_task();
    test_task_exit_defers_its_own_reclamation();
    TEST_REPORT("task");
}
