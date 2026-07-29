#include "test.h"
#include "../timer.h"
#include "../isr.h"
#include "../task.h"

#include <stddef.h>
#include <stdlib.h>
#include <setjmp.h>

/*
 * The timer's interesting logic is the wake-up test tick_reached(), which
 * compares a 32-bit tick counter against a deadline using a SIGNED difference
 * so that it stays correct across the counter's wrap at 2^32. A naive unsigned
 * `current >= deadline` would fire a sleep immediately the moment the counter
 * wrapped past the deadline -- a bug that only manifests after ~497 days of
 * uptime at 100 Hz, i.e. never observable from the shell but very real.
 *
 * timer_callback (which advances the tick and wakes due sleepers) is static, so
 * the test captures it the way the kernel does: the stubbed
 * register_interrupt_handler records the handler timer_install() registers, and
 * the test then calls it to simulate timer interrupts. The privileged port I/O
 * in timer_install() is a no-op under HOSTED_TEST.
 *
 * The other half of the file covers timer_sleep's kill handling (F19). A sleep
 * is the one wait that cannot use task_block_killable(), because the sleep slot
 * has to be handed back before the task disappears -- otherwise it stays
 * reserved, pointing at a freed task_t, until the original deadline passes. The
 * release is deliberately conditional: if the deadline already fired,
 * timer_callback cleared the slot and another sleeper may now own it, so
 * clearing unconditionally would steal that one's slot instead.
 */

/* timer.c owns this global; the test drives it directly to reach the wrap. */
extern uint32_t timer_ticks;

/* --- captured interrupt handler + scheduler/process stubs ----------------- */
static isr_t g_tick;                 /* timer_callback, captured at install */
static int   g_wakes;                /* task_wake_all calls */
static task_t *g_current = (task_t *)0x1234;   /* non-NULL fake "current task" */

void register_interrupt_handler(uint8_t n, isr_t handler) {
    if (n == 32) g_tick = handler;
}

task_t *task_get_current(void) { return g_current; }
void task_wake_all(const void *chan) { (void)chan; g_wakes++; }

/* --- kill plumbing (F19) -------------------------------------------------- */
/* timer_sleep checks for a pending kill before taking a sleep slot and again
 * after waking, so both the flag and the exit have to be drivable from here. */
static int     g_kill_pending;     /* what task_kill_pending() reports */
static jmp_buf g_exit_jmp;         /* where task_exit() lands, when armed */
static int     g_exit_armed;
static int32_t g_exit_status;

int task_kill_pending(void) { return g_kill_pending; }

/* task_exit() is noreturn in the kernel: the task is unlinked and switched away
 * from, never resuming. longjmp reproduces that here -- control leaves
 * timer_sleep and never comes back to the statement after the call. Reaching it
 * unarmed is a real failure, as in the tests that must never be killed. */
void task_exit(int32_t status) {
    g_exit_status = status;
    if (g_exit_armed) longjmp(g_exit_jmp, 1);
    printf("  FAIL unexpected task_exit(%d)\n", status);
    exit(1);
}

static void tick(void);

/* Hooks that fire from inside the park, which is the only moment a kill can
 * realistically land on a sleeping task. */
static int g_kill_on_block;   /* a kill request arrives while parked */
static int g_takeover;        /* ...but the deadline fired first and another
                                 task has since taken over the slot */
static int g_blocks;          /* how many times the sleep actually parked */

void task_block_current(const void *chan) {
    (void)chan;
    g_blocks++;

    if (g_takeover) {
        g_takeover = 0;               /* one shot: the inner sleep parks normally */
        /* Our deadline fires: timer_callback wakes us and clears our slot... */
        while (timer_get_sleeping_count() > 0) tick();
        /* ...and an unrelated task claims the slot we just gave up. */
        g_current = (task_t *)0x5678;
        (void)timer_sleep(1000);
        g_current = (task_t *)0x1234; /* back to the task being killed */
        /* Flag the kill only now, so it lands on us and not on the new owner
         * (whose sleep above must park and return normally). */
        g_kill_pending = 1;
    }
    if (g_kill_on_block) g_kill_pending = 1;
}

/* Run timer_sleep(ticks) expecting the kill check to terminate the task.
 * Returns 1 if task_exit() was reached, 0 if timer_sleep returned normally. */
static int sleep_expecting_exit(uint32_t ticks) {
    g_exit_armed = 1;
    g_exit_status = 0;
    if (setjmp(g_exit_jmp) != 0) {    /* arrived here via task_exit() */
        g_exit_armed = 0;
        return 1;
    }
    (void)timer_sleep(ticks);
    g_exit_armed = 0;
    return 0;
}

/* timer_callback also drives these every tick; nothing here needs them to act. */
void schedule(void) {}
void process_account_tick(void) {}
void process_check_kill(void) {}
void process_check_alarms(uint32_t t) { (void)t; }

/* Simulate one timer interrupt. */
static void tick(void) { g_tick(NULL); }


static void reset(uint32_t start) {
    /* Drain any leftover sleepers from a previous test, then set the clock. The
     * loop is bounded: if a (mutated) callback failed to release a woken slot
     * this would otherwise spin forever, so cap it and let the test fail on its
     * own assertions instead of hanging. */
    int guard = 0;
    while (timer_get_sleeping_count() > 0 && guard++ < 64) {
        /* Advance far enough that every possible deadline is reached. */
        timer_ticks += 0x40000000u;
        tick();
    }
    timer_ticks = start;
    g_wakes = 0;
    g_current = (task_t *)0x1234;
    g_kill_pending = 0;
    g_kill_on_block = 0;
    g_takeover = 0;
    g_blocks = 0;
}

static void test_install_captured(void) {
    TEST("install captures handler");
    timer_install();
    CHECK(g_tick != NULL);
}

static void test_sleep_validation(void) {
    reset(0);
    TEST("sleep argument validation");
    CHECK_EQ(timer_sleep(0), 0);                 /* zero ticks: no-op */
    CHECK_EQ(timer_get_sleeping_count(), 0);     /* and no slot consumed */
    CHECK_EQ(timer_sleep(0x80000000u), -1);      /* too large */
    CHECK_EQ(timer_get_sleeping_count(), 0);

    g_current = NULL;                            /* no current task */
    CHECK_EQ(timer_sleep(5), -1);
    g_current = (task_t *)0x1234;
}

static void test_basic_wake(void) {
    reset(100);
    TEST("wakes exactly at the deadline");
    CHECK_EQ(timer_sleep(5), 0);                 /* wake_tick = 105 */
    CHECK_EQ(timer_get_sleeping_count(), 1);

    for (int i = 0; i < 4; i++) tick();          /* ticks 101..104 */
    CHECK_EQ(timer_get_sleeping_count(), 1);     /* not yet */
    CHECK_EQ(g_wakes, 0);

    tick();                                       /* tick 105 */
    CHECK_EQ(g_wakes, 1);
    CHECK_EQ(timer_get_sleeping_count(), 0);     /* slot released */
    CHECK_EQ(timer_get_ticks(), 105);
}

static void test_wraparound(void) {
    /* The whole point: the tick counter wraps through 0 between the sleep and
     * the deadline. A signed-difference comparison stays correct; an unsigned
     * one would fire early at 0xFFFFFFFF. */
    reset(0xFFFFFFFEu);
    TEST("deadline correct across the 2^32 wrap");
    CHECK_EQ(timer_sleep(3), 0);                 /* wake_tick = 0xFFFFFFFE+3 = 1 */
    CHECK_EQ(timer_get_sleeping_count(), 1);

    tick();                                       /* 0xFFFFFFFF: must NOT wake */
    CHECK_EQ(g_wakes, 0);
    CHECK_EQ(timer_get_sleeping_count(), 1);

    tick();                                       /* 0x00000000: still not due */
    CHECK_EQ(g_wakes, 0);
    CHECK_EQ(timer_get_sleeping_count(), 1);

    tick();                                       /* 0x00000001: now due */
    CHECK_EQ(g_wakes, 1);
    CHECK_EQ(timer_get_sleeping_count(), 0);
}

static void test_multiple_sleepers(void) {
    reset(1000);
    TEST("independent deadlines");
    CHECK_EQ(timer_sleep(3), 0);      /* due at 1003 */
    CHECK_EQ(timer_sleep(1), 0);      /* due at 1001 */
    CHECK_EQ(timer_sleep(5), 0);      /* due at 1005 */
    CHECK_EQ(timer_get_sleeping_count(), 3);

    tick();                            /* 1001: the second one */
    CHECK_EQ(g_wakes, 1);
    CHECK_EQ(timer_get_sleeping_count(), 2);

    tick(); tick();                    /* 1002, 1003: the first one */
    CHECK_EQ(g_wakes, 2);
    CHECK_EQ(timer_get_sleeping_count(), 1);

    tick(); tick();                    /* 1004, 1005: the third one */
    CHECK_EQ(g_wakes, 3);
    CHECK_EQ(timer_get_sleeping_count(), 0);
}

static void test_slot_exhaustion(void) {
    int i;
    reset(0);
    TEST("sleep table is bounded");
    /* MAX_SLEEPING_TASKS is 16 (private); fill until it refuses. */
    for (i = 0; i < 16; i++) CHECK_EQ(timer_sleep(1000 + i), 0);
    CHECK_EQ(timer_get_sleeping_count(), 16);
    CHECK_EQ(timer_sleep(1), -1);      /* full -> rejected, not overrun */
    CHECK_EQ(timer_get_sleeping_count(), 16);
}

static void test_kill_before_sleep(void) {
    reset(0);
    TEST("a kill already pending exits instead of taking a sleep slot");
    /* Taking a slot here would reserve it for a task that is about to die, and
     * nothing would ever wake it. */
    g_kill_pending = 1;
    CHECK_EQ(sleep_expecting_exit(50), 1);
    CHECK_EQ(g_exit_status, TASK_KILL_STATUS);
    CHECK_EQ(timer_get_sleeping_count(), 0);
    /* It must not park at all. Without this the post-block check would mask a
     * missing pre-block one: both exit with the slot released, and the only
     * difference is that the task needlessly parked first -- relying on a wake
     * that, for an already-doomed task, may never come. */
    CHECK_EQ(g_blocks, 0);
}

static void test_kill_while_parked_releases_slot(void) {
    reset(500);
    TEST("a kill landing while parked releases the sleep slot");
    g_kill_on_block = 1;
    CHECK_EQ(sleep_expecting_exit(50), 1);
    CHECK_EQ(g_exit_status, TASK_KILL_STATUS);
    CHECK_EQ(g_blocks, 1);        /* it really did park: the post-block path */
    /* The dying task must hand the slot back. Leaking it would both waste one
     * of the 16 slots and leave a pointer to a freed task_t that the callback
     * would later dereference. */
    CHECK_EQ(timer_get_sleeping_count(), 0);
}

static void test_kill_does_not_steal_a_reused_slot(void) {
    reset(500);
    TEST("a killed sleeper does not clear a slot someone else now owns");
    /* Our deadline fires while we are parked, so timer_callback has already
     * released our slot and another task has taken it. The dying task must
     * leave that one alone -- clearing unconditionally would cancel an
     * unrelated sleep, which never wakes and never gives its slot back. */
    g_takeover = 1;               /* sets the kill itself, once the slot is reused */
    CHECK_EQ(sleep_expecting_exit(50), 1);
    CHECK_EQ(g_exit_status, TASK_KILL_STATUS);
    CHECK_EQ(timer_get_sleeping_count(), 1);   /* the new owner's sleep survives */
}

static void test_normal_sleep_still_returns(void) {
    reset(0);
    TEST("a sleep with no kill pending still returns normally");
    /* Guards the reverse mistake: the kill checks must not fire on their own. */
    CHECK_EQ(sleep_expecting_exit(10), 0);
    CHECK_EQ(timer_get_sleeping_count(), 1);
}

int main(void) {
    test_install_captured();
    test_sleep_validation();
    test_basic_wake();
    test_wraparound();
    test_multiple_sleepers();
    test_slot_exhaustion();
    test_kill_before_sleep();
    test_kill_while_parked_releases_slot();
    test_kill_does_not_steal_a_reused_slot();
    test_normal_sleep_still_returns();
    TEST_REPORT("timer");
}
