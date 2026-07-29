#include "user_syscall.h"

/*
 * killwait - regression test for killing tasks that are parked in a blocking
 * wait.
 *
 * A kill request used to be applied only by the per-tick check that runs on
 * whichever task is current (process_check_kill). A task sitting in
 * `while (cond) task_block_current(ch);` never becomes current -- it only runs
 * the handful of instructions between a wake and the next block, with
 * interrupts disabled, so no timer tick ever lands on it. Such a task stayed
 * asleep for good: its process remained RUNNING forever, and anything waiting
 * on that process blocked forever too.
 *
 * Both tasks here are parked in waits that are never satisfied when the kill
 * arrives from outside:
 *   - the worker thread waits on a semaphore nobody posts;
 *   - main waits in sys_thread_join() for that worker.
 * The kill comes from a forked child, so it lands while both are asleep. Both
 * must die: neither SURVIVED line may appear, and the suite's end-of-run `mem`
 * assertions (Processes: running=0, Tasks: blocked=0) must still hold.
 *
 * Launched in the background so a regression shows up in those assertions
 * rather than hanging the shell on a process that can never exit.
 */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

#define SEM_ID           4    /* not one of semtest's (0, 1) */
#define STACK_PAGES      4
#define KILL_DELAY_TICKS 60   /* 100 Hz: ~0.6s, ample time for both parks */

static void worker(void) {
    sys_sem_wait(SEM_ID);   /* nothing ever posts this: parks for good */
    write_str("[killwait worker SURVIVED]\n");
    sys_exit(0);
}

int main(void) {
    int self = sys_getpid();
    char *stack;
    int pid;

    if (sys_sem_init(SEM_ID, 0) < 0) {
        write_str("[killwait] sem_init failed\n");
        return 1;
    }

    pid = sys_fork();
    if (pid < 0) {
        write_str("[killwait] fork failed\n");
        return 1;
    }
    if (pid == 0) {
        /* The killer: an unrelated process, so the request arrives while every
         * task of the target is blocked. */
        sys_sleep(KILL_DELAY_TICKS);
        sys_kill(self, SIGKILL);
        write_str("[killwait killer done]\n");
        sys_exit(0);
    }

    stack = (char *)sys_mmap(STACK_PAGES);
    if (!stack) {
        write_str("[killwait] mmap failed\n");
        return 1;
    }
    if (sys_thread_create(worker, stack + STACK_PAGES * 4096) < 0) {
        write_str("[killwait] thread_create failed\n");
        return 1;
    }

    write_str("[killwait parked]\n");
    sys_thread_join();   /* the worker never exits: parks for good */

    /* Only reached if the kill failed to reach a parked task. */
    write_str("[killwait parent SURVIVED]\n");
    return 0;
}
