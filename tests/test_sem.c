#include "test.h"
#include "../sem.h"

#include <limits.h>
#include <stdlib.h>

/*
 * Counting semaphores. The value accounting and the id validation run without
 * blocking and are checked directly. The blocking wait (value <= 0) is driven
 * by a scripted hook that stands in for another task calling sem_post, so the
 * "block until positive, then decrement" loop is exercised on one thread.
 */

static void  (*g_block_hook)(const void *);
static int     g_block_count;
static int     g_post_id;    /* id the hook should post to release a waiter */

void task_block_current(const void *chan) {
    g_block_count++;
    if (g_block_hook) { g_block_hook(chan); return; }
    /* No hook: an unexpected block. Post the id so the wait loop can exit and
     * the test fails on the block-count assertion rather than hanging. */
    sem_post(g_post_id);
}
void task_wake_one(const void *chan) { (void)chan; }
void task_wake_all(const void *chan) { (void)chan; }

/* task_block_killable() consults these on both sides of every block. Nothing
 * in this harness issues a kill request, so sem_wait must never exit. */
int task_kill_pending(void) { return 0; }
void task_exit(int32_t status) {
    printf("  FAIL unexpected task_exit(%d)\n", status);
    exit(1);
}

static void test_id_validation(void) {
    TEST("id validation");
    CHECK_EQ(sem_init(-1, 1), -1);
    CHECK_EQ(sem_init(MAX_SEMAPHORES, 1), -1);
    CHECK_EQ(sem_init(0, -1), -1);          /* negative initial value */
    CHECK_EQ(sem_wait(-1), -1);
    CHECK_EQ(sem_wait(MAX_SEMAPHORES), -1);
    CHECK_EQ(sem_post(-1), -1);
    CHECK_EQ(sem_post(MAX_SEMAPHORES), -1);

    CHECK_EQ(sem_init(0, 1), 0);            /* a valid one still works */
}

static void test_uninitialised(void) {
    /* A semaphore that was never initialised must reject wait/post: id 5 is
     * left untouched by every other test. */
    TEST("uninitialised rejected");
    CHECK_EQ(sem_wait(5), -1);
    CHECK_EQ(sem_post(5), -1);
}

static void test_counting(void) {
    g_block_count = 0;
    g_block_hook = NULL;

    TEST("counting without blocking");
    CHECK_EQ(sem_init(1, 3), 0);
    /* Three waits drain the count without ever blocking. */
    CHECK_EQ(sem_wait(1), 0);
    CHECK_EQ(sem_wait(1), 0);
    CHECK_EQ(sem_wait(1), 0);
    CHECK_EQ(g_block_count, 0);

    /* Posts raise it again; a subsequent wait consumes without blocking. */
    CHECK_EQ(sem_post(1), 0);
    CHECK_EQ(sem_post(1), 0);
    CHECK_EQ(sem_wait(1), 0);
    CHECK_EQ(sem_wait(1), 0);
    CHECK_EQ(g_block_count, 0);
}

/* Peer action: release the waiter by posting its id once. */
static void hook_post_once(const void *chan) {
    (void)chan;
    g_block_hook = NULL;      /* one shot: the next block (if any) is a failure */
    sem_post(g_post_id);
}

static void test_wait_blocks_then_posted(void) {
    TEST("wait blocks on zero, then a post releases it");
    CHECK_EQ(sem_init(2, 0), 0);           /* starts empty */
    g_block_count = 0;
    g_post_id = 2;
    g_block_hook = hook_post_once;

    /* value is 0, so wait blocks once; the hook posts (value 1); the loop
     * re-checks, finds it positive, decrements to 0 and returns. */
    CHECK_EQ(sem_wait(2), 0);
    CHECK_EQ(g_block_count, 1);

    /* The value must be back to 0: exactly one post, exactly one decrement. */
    g_block_count = 0;
    g_post_id = 2;
    g_block_hook = hook_post_once;
    CHECK_EQ(sem_wait(2), 0);              /* blocks again -> proves it was 0 */
    CHECK_EQ(g_block_count, 1);
}

static void test_reinit_resets_value(void) {
    TEST("re-init resets the value");
    CHECK_EQ(sem_init(3, 5), 0);
    CHECK_EQ(sem_wait(3), 0);              /* value 4 */
    CHECK_EQ(sem_init(3, 1), 0);          /* reset to 1 */
    g_block_count = 0;
    g_post_id = 3;
    g_block_hook = hook_post_once;
    CHECK_EQ(sem_wait(3), 0);             /* consumes the 1 without blocking */
    CHECK_EQ(g_block_count, 0);
    CHECK_EQ(sem_wait(3), 0);             /* now 0 -> blocks once */
    CHECK_EQ(g_block_count, 1);
}

static void test_post_rejects_count_overflow(void) {
    int rc;

    TEST("post rejects count overflow");
    CHECK_EQ(sem_init(4, INT_MAX), 0);
    rc = sem_post(4);
    CHECK_EQ(rc, -1);

    /* On a correct rejection the count is still usable and still at INT_MAX.
     * Skip this path on the broken implementation, whose wrapped-negative
     * value would make sem_wait block for billions of posts. */
    if (rc == -1) {
        g_block_count = 0;
        CHECK_EQ(sem_wait(4), 0);
        CHECK_EQ(g_block_count, 0);
        CHECK_EQ(sem_post(4), 0);
        CHECK_EQ(sem_post(4), -1);
    }
}

int main(void) {
    test_id_validation();
    test_uninitialised();
    test_counting();
    test_wait_blocks_then_posted();
    test_reinit_resets_value();
    test_post_rejects_count_overflow();
    TEST_REPORT("sem");
}
