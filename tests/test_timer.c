#include "test.h"
#include "../timer.h"
#include "../isr.h"
#include "../task.h"

#include <stddef.h>

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
void task_block_current(const void *chan) { (void)chan; }   /* sleep just parks */
void task_wake_all(const void *chan) { (void)chan; g_wakes++; }

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

int main(void) {
    test_install_captured();
    test_sleep_validation();
    test_basic_wake();
    test_wraparound();
    test_multiple_sleepers();
    test_slot_exhaustion();
    TEST_REPORT("timer");
}
