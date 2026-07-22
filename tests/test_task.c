#include "test.h"
#include "../task.h"
#include "../pmm.h"

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
 *     end-to-end test.
 *
 * All of that runs without a real context switch, so it can be checked here
 * directly.
 */

/* --- stubs for everything task.c touches except the list logic ------------ */
static uint8_t pool[80][PMM_BLOCK_SIZE] __attribute__((aligned(PMM_BLOCK_SIZE)));
static int pool_next;

void *pmm_alloc_block(void) {
    return pool_next < (int)(sizeof(pool) / sizeof(pool[0])) ? pool[pool_next++] : NULL;
}
void pmm_free_block(void *p) { (void)p; }          /* leak in the test; fine */

void switch_task(uint32_t *old_esp, uint32_t new_esp) { (void)old_esp; (void)new_esp; }
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

int main(void) {
    test_setup();
    test_block_removes_from_ready();
    test_fifo_wake_order();
    test_wake_task_by_identity();
    test_wake_channel_selectivity();
    TEST_REPORT("task");
}
