#include "user_syscall.h"

/*
 * killthread - regression test for killing a multi-threaded process.
 *
 * process_check_kill() terminates whichever task of the target process happens
 * to be current, and it used to clear the pending kill request right there. A
 * process with extra threads therefore lost only one task: the siblings kept
 * running with the request gone, so the process stayed RUNNING forever and
 * anything waiting on it blocked forever.
 *
 * Here main starts a worker that stays runnable (a busy loop, so a timer tick
 * is guaranteed to land while it is current) and then kills its own process.
 * Both tasks must die. The program is launched in the background so nothing
 * blocks on it; a surviving thread shows up in the suite's end-of-run `mem`
 * assertions (Processes: running=0, Tasks: blocked=0) rather than as a hang.
 */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

#define STACK_PAGES 4

static void worker(void) {
    /* Stay schedulable rather than parking in a wait: the kill is applied to
     * whichever task is current on a timer tick. */
    volatile unsigned spin = 0;
    for (;;) spin++;
}

int main(void) {
    char *stk = (char *)sys_mmap(STACK_PAGES);
    if (!stk) { write_str("[killthread] mmap failed\n"); return 1; }

    if (sys_thread_create(worker, stk + STACK_PAGES * 4096) < 0) {
        write_str("[killthread] create failed\n");
        return 1;
    }

    write_str("[killthread armed]\n");
    sys_kill(sys_getpid(), SIGKILL);   /* must take the worker down too */

    /* Only reached if the kill failed to terminate this task. */
    for (volatile unsigned i = 0; i < 40000000u; i++) { }
    write_str("[killthread SURVIVED]\n");
    return 0;
}
