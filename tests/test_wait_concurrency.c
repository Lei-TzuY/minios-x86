/* Focused lifecycle regressions for concurrent waiters.
 *
 * Reuse the full scheduler/process harness from test_process.c, but replace its
 * main so this gate can drive races that need a second reap while the first
 * waiter is parked and a waitpid issued by a sibling thread. */
#define main process_regression_main
#include "test_process.c"
#undef main

typedef struct reap_race {
    process_t *child;
    int32_t child_pid;
} reap_race_t;

static void child_exits_then_other_waiter_reaps(void *arg) {
    reap_race_t *race = (reap_race_t *)arg;
    int32_t status = 0;

    exit_task(race->child->task, 7);
    CHECK_EQ(process_waitpid(race->child_pid, &status, 1), race->child_pid);
    CHECK_EQ(status, 7);
}

static task_t *task_by_id(int32_t id) {
    for (int i = 0; i < g_task_count; i++) {
        if (g_task_used[i] && (int32_t)g_tasks[i].id == id) return &g_tasks[i];
    }
    return NULL;
}

static void test_wait_revalidates_after_another_waiter_reaps(void) {
    process_t *parent, *child;
    reap_race_t race;
    int release_before;

    TEST("wait rejects a child already reaped while it was blocked");
    reset_world();
    parent = launch("parent", 0);
    child = launch("child", 1);
    CHECK(parent && child);
    if (!parent || !child) return;

    child->parent_pid = parent->pid;
    race.child = child;
    race.child_pid = child->pid;
    g_current_task = parent->task;
    g_on_block = child_exits_then_other_waiter_reaps;
    g_on_block_arg = &race;
    g_on_block_remaining = 1;
    release_before = g_process_release_calls;

    /* The other waiter owns the successful reap. This stale waiter must see
     * that its target disappeared and return -1 without releasing the slot a
     * second time. */
    CHECK_EQ(process_wait(race.child_pid), -1);
    CHECK_EQ(g_process_release_calls - release_before, 1);
    CHECK_EQ(used_slots(), 1);              /* only the parent remains */
}

static void test_thread_waitpid_is_woken_on_child_event_channel(void) {
    process_t *parent, *child;
    task_t *thread;
    int32_t tid;
    int32_t status = 0;
    int32_t child_pid;

    TEST("waitpid sibling wakes through the dedicated child-event channel");
    reset_world();
    parent = launch("parent", 0);
    child = launch("child", 1);
    CHECK(parent && child);
    if (!parent || !child) return;
    child->parent_pid = parent->pid;
    child_pid = child->pid;

    g_current_task = parent->task;
    tid = process_thread_create(0x4000, 0x8000);
    CHECK(tid > 0);
    thread = task_by_id(tid);
    CHECK(thread != NULL);
    if (!thread) return;

    /* process->task is still the main task. The waiting sibling therefore
     * cannot rely on SIGCHLD's identity wake of parent->task; child exit must
     * also broadcast on the dedicated parent child-event wait channel. */
    CHECK(parent->task != thread);
    g_current_task = thread;
    g_on_block = child_exits;
    g_on_block_arg = child;
    g_on_block_remaining = 1;

    CHECK_EQ(process_waitpid(child_pid, &status, 0), child_pid);
    CHECK_EQ(status, 7);
    CHECK_EQ(g_block_calls, 1);
    CHECK_EQ(g_stuck_blocks, 0);
    CHECK_EQ(g_hang_detected, 0);
}

int main(void) {
    test_wait_revalidates_after_another_waiter_reaps();
    test_thread_waitpid_is_woken_on_child_event_channel();
    TEST_REPORT("wait-concurrency");
}
