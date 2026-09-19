#include "test.h"
#include "../timer.h"
#include "../isr.h"
#include "../task.h"

#include <pthread.h>
#include <stdlib.h>
#include <time.h>

/* Real suspended C stacks, with deterministic scheduling: the driver and all
 * sleepers hold one mutex whenever kernel code runs. Only task_block_current
 * drops it, exactly where the single-core kernel can switch to another task.
 * A wake makes a task eligible to resume; allow_return can delay that resumption
 * so a different task reuses the expired slot first. No host timing drives ticks.
 *
 * The old stub returned from task_block_current while pretending the task was
 * still asleep. That hid the resource leak on non-kill early returns and cannot
 * model the slot-owner race once all return paths release their reservations.
 */
#define SLEEPERS 17

typedef struct {
    task_t task;
    pthread_t host_thread;
    const void *channel;
    uint32_t ticks;
    int active, blocked, woken, allow_return, done, killed;
    int result;
} sleeper_t;

extern uint32_t timer_ticks;
static sleeper_t sleepers[SLEEPERS];
static __thread sleeper_t *current;
static pthread_mutex_t cpu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static isr_t g_tick;
static int g_wakes, g_blocks;

void register_interrupt_handler(uint8_t n, isr_t handler) {
    if (n == 32) g_tick = handler;
}

task_t *task_get_current(void) { return current ? &current->task : NULL; }
int task_kill_pending(void) { return current && current->task.kill_pending; }
void schedule(void) {}
void process_account_tick(void) {}
void process_check_kill(void) {}
void process_check_alarms(uint32_t ticks) { (void)ticks; }

void task_wake_all(const void *channel) {
    g_wakes++;
    for (int i = 0; i < SLEEPERS; i++) {
        if (sleepers[i].active && sleepers[i].blocked &&
            sleepers[i].channel == channel) sleepers[i].woken = 1;
    }
    pthread_cond_broadcast(&changed);
}

void task_block_current(const void *channel) {
    current->channel = channel;
    current->blocked = 1;
    g_blocks++;
    pthread_cond_broadcast(&changed);
    while (!current->woken || !current->allow_return)
        pthread_cond_wait(&changed, &cpu);
    current->blocked = 0;
}

void task_exit(int32_t status) {
    current->killed = 1;
    current->result = status;
    current->done = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&cpu);
    pthread_exit(NULL);
}

static void *sleep_worker(void *arg) {
    pthread_mutex_lock(&cpu);
    current = arg;
    current->result = timer_sleep(current->ticks);
    current->done = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&cpu);
    return NULL;
}

/* A broken harness must fail with a name, rather than hang the native gate. */
static void await_state(sleeper_t *s, int completion) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;
    while (!s->done && (completion || !s->blocked)) {
        if (pthread_cond_timedwait(&changed, &cpu, &deadline) != 0) {
            CHECK(!"timer harness failed to reach the requested task state");
            exit(1);
        }
    }
}

static sleeper_t *start_sleep(int index, uint32_t ticks, int killed, int defer) {
    sleeper_t *s = &sleepers[index];
    CHECK(!s->active);
    *s = (sleeper_t){ .active = 1, .ticks = ticks, .allow_return = !defer };
    s->task.kill_pending = killed;
    if (pthread_create(&s->host_thread, NULL, sleep_worker, s) != 0) {
        CHECK(!"timer harness could not create a task");
        exit(1);
    }
    await_state(s, 0);
    return s;
}

static void finish_sleep(sleeper_t *s, int result, int killed) {
    /* Missing wakeups are asserted before forcing progress, so a deadline
     * mutation produces a useful failure instead of a blocked pthread_join. */
    CHECK(s->done || s->woken);
    s->woken = s->allow_return = 1;
    pthread_cond_broadcast(&changed);
    await_state(s, 1);
    CHECK_EQ(s->result, result);
    CHECK_EQ(s->killed, killed);
    pthread_join(s->host_thread, NULL);
    s->active = 0;
}

static void early_wake(sleeper_t *s, int killed) {
    s->task.kill_pending = killed;
    s->woken = 1;  /* task_wake_task from a signal, not the timer callback */
    finish_sleep(s, killed ? TASK_KILL_STATUS : 0, killed);
}

static void tick(void) { g_tick(NULL); }

static void reset(uint32_t start) {
    for (int i = 0; i < SLEEPERS; i++) CHECK(!sleepers[i].active);
    /* Drain only after all task stacks have returned. This keeps later cases
     * independent when testing a mutant that intentionally leaks a slot. */
    for (int i = 0; i < 4 && timer_get_sleeping_count(); i++) {
        timer_ticks += 0x40000000u;
        tick();
    }
    CHECK_EQ(timer_get_sleeping_count(), 0);
    timer_ticks = start;
    g_wakes = g_blocks = 0;
}

static void test_sleep_validation(void) {
    reset(0);
    TEST("sleep argument validation");
    CHECK_EQ(timer_sleep(0), 0);
    CHECK_EQ(timer_sleep(0x80000000u), -1);
    CHECK_EQ(timer_sleep(5), -1); /* no current task in the driver */
    CHECK_EQ(timer_get_sleeping_count(), 0);
    finish_sleep(start_sleep(0, 0, 0, 0), 0, 0);
    finish_sleep(start_sleep(0, 0x80000000u, 0, 0), -1, 0);
    CHECK_EQ(g_blocks, 0);
}

static void test_basic_wake(void) {
    reset(100);
    TEST("wakes exactly at the deadline");
    sleeper_t *s = start_sleep(0, 5, 0, 0);
    CHECK_EQ(timer_get_sleeping_count(), 1);
    for (int i = 0; i < 4; i++) {
        tick();
        CHECK_EQ(timer_get_sleeping_count(), 1);
        CHECK_EQ(g_wakes, 0);
        CHECK(!s->woken);
    }
    tick();
    CHECK_EQ(g_wakes, 1);
    CHECK_EQ(timer_get_sleeping_count(), 0);
    CHECK_EQ(timer_get_ticks(), 105);
    finish_sleep(s, 0, 0);
}

static void test_wraparound(void) {
    reset(0xFFFFFFFEu);
    TEST("deadline correct across the 2^32 wrap");
    sleeper_t *s = start_sleep(0, 3, 0, 0);
    for (int i = 0; i < 2; i++) {
        tick();
        CHECK_EQ(g_wakes, 0);
        CHECK_EQ(timer_get_sleeping_count(), 1);
        CHECK(!s->woken);
    }
    tick();
    CHECK_EQ(timer_get_ticks(), 1);
    CHECK_EQ(g_wakes, 1);
    CHECK_EQ(timer_get_sleeping_count(), 0);
    finish_sleep(s, 0, 0);
}

static void test_maximum_deadline(void) {
    reset(0x90000000u);
    TEST("largest accepted sleep keeps its full deadline across wrap");
    sleeper_t *s = start_sleep(0, 0x7FFFFFFFu, 0, 0);
    tick();
    CHECK_EQ(g_wakes, 0);
    timer_ticks = 0x0FFFFFFDu;
    tick();
    CHECK_EQ(g_wakes, 0);
    CHECK_EQ(timer_get_sleeping_count(), 1);
    tick();
    CHECK_EQ(g_wakes, 1);
    CHECK_EQ(timer_get_sleeping_count(), 0);
    finish_sleep(s, 0, 0);
}

static void test_multiple_sleepers(void) {
    reset(1000);
    TEST("independent tasks retain independent deadlines");
    sleeper_t *a = start_sleep(0, 3, 0, 0);
    sleeper_t *b = start_sleep(1, 1, 0, 0);
    sleeper_t *c = start_sleep(2, 5, 0, 0);
    CHECK_EQ(timer_get_sleeping_count(), 3);
    tick();
    CHECK_EQ(g_wakes, 1);
    CHECK_EQ(timer_get_sleeping_count(), 2);
    CHECK(!a->woken && !c->woken);
    finish_sleep(b, 0, 0);
    tick(); tick();
    CHECK_EQ(g_wakes, 2);
    CHECK_EQ(timer_get_sleeping_count(), 1);
    CHECK(!c->woken);
    finish_sleep(a, 0, 0);
    tick(); tick();
    CHECK_EQ(g_wakes, 3);
    CHECK_EQ(timer_get_sleeping_count(), 0);
    finish_sleep(c, 0, 0);
}

static void test_slot_exhaustion(void) {
    reset(0);
    TEST("16 real suspended sleeps exhaust the table and one wake restores capacity");
    for (int i = 0; i < 16; i++) start_sleep(i, 1000 + i, 0, 0);
    CHECK_EQ(timer_get_sleeping_count(), 16);
    finish_sleep(start_sleep(16, 1, 0, 0), -1, 0);
    CHECK_EQ(g_blocks, 16);
    CHECK_EQ(timer_get_sleeping_count(), 16);
    early_wake(&sleepers[7], 0);
    CHECK_EQ(timer_get_sleeping_count(), 15);
    sleeper_t *replacement = start_sleep(16, 1, 0, 0);
    CHECK_EQ(timer_get_sleeping_count(), 16);
    tick();
    finish_sleep(replacement, 0, 0);
    CHECK_EQ(timer_get_sleeping_count(), 15);
    for (int i = 0; i < 16; i++) {
        if (i != 7) early_wake(&sleepers[i], 0);
    }
    CHECK_EQ(timer_get_sleeping_count(), 0);
}

static void test_repeated_early_wake(void) {
    reset(0);
    TEST("non-kill early wakes immediately release all sleep slots");
    for (int i = 0; i < 32; i++) {
        early_wake(start_sleep(0, 0x7FFFFFFFu, 0, 0), 0);
        CHECK_EQ(timer_get_sleeping_count(), 0);
    }
    CHECK_EQ(g_blocks, 32);
    timer_ticks = 0x7FFFFFFEu;
    tick();
    CHECK_EQ(g_wakes, 0); /* canceled deadlines must not fire later */
    sleeper_t *s = start_sleep(0, 1, 0, 0);
    tick();
    finish_sleep(s, 0, 0);
    CHECK_EQ(timer_get_sleeping_count(), 0);
}

static void test_kill_paths(void) {
    reset(500);
    TEST("pending kill exits before reserving or blocking");
    finish_sleep(start_sleep(0, 50, 1, 0), TASK_KILL_STATUS, 1);
    CHECK_EQ(timer_get_sleeping_count(), 0);
    CHECK_EQ(g_blocks, 0);

    TEST("kill during sleep releases the slot before task exit");
    early_wake(start_sleep(0, 50, 0, 0), 1);
    CHECK_EQ(g_blocks, 1);
    CHECK_EQ(timer_get_sleeping_count(), 0);
    timer_ticks = 549;
    tick();
    CHECK_EQ(g_wakes, 0);
}

static void test_slot_takeover(int killed) {
    reset(500);
    TEST(killed ? "killed sleeper leaves a reused slot intact" :
                  "normal return leaves a reused slot intact");
    sleeper_t *old = start_sleep(0, 1, 0, 1);
    tick(); /* old is ready, but cannot run until explicitly permitted */
    CHECK_EQ(timer_get_sleeping_count(), 0);
    CHECK(old->woken);
    sleeper_t *next = start_sleep(1, 3, 0, 0);
    CHECK(old->channel == next->channel); /* prove actual slot reuse */
    old->task.kill_pending = killed;
    finish_sleep(old, killed ? TASK_KILL_STATUS : 0, killed);
    CHECK_EQ(timer_get_sleeping_count(), 1);
    tick(); tick();
    CHECK_EQ(timer_get_sleeping_count(), 1);
    CHECK(!next->woken);
    tick();
    CHECK_EQ(g_wakes, 2);
    CHECK_EQ(timer_get_sleeping_count(), 0);
    finish_sleep(next, 0, 0);
}

int main(void) {
    TEST("install captures handler");
    timer_install();
    CHECK(g_tick != NULL);
    pthread_mutex_lock(&cpu);
    test_sleep_validation();
    test_basic_wake();
    test_wraparound();
    test_maximum_deadline();
    test_multiple_sleepers();
    test_slot_exhaustion();
    test_repeated_early_wake();
    test_kill_paths();
    test_slot_takeover(0);
    test_slot_takeover(1);
    pthread_mutex_unlock(&cpu);
    TEST_REPORT("timer");
}
