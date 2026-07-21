#include "user_syscall.h"

/*
 * threadexit - regression test for a fixed kernel bug: the main thread of a
 * process returning/exit()ing while a sibling thread (SYS_THREAD_CREATE) was
 * still running used to tear down the shared address space out from under
 * that thread. The next time the scheduler picked the worker back up, it
 * would load a freed page directory into CR3 (use-after-free -> corruption
 * or an immediate reboot).
 *
 * This program deliberately does NOT call sys_thread_join(): main exits
 * immediately after starting the worker, and the worker keeps running well
 * past that point before exiting on its own. If the bug were still present,
 * miniOS would crash (the QEMU test harness would see the log stop dead) as
 * soon as the scheduler next picked the worker task. With the fix, the
 * worker keeps running normally in the (still-alive) shared address space,
 * and the process only becomes reapable once the worker itself exits.
 */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

static void worker(void) {
    /* Keep running well after main has already exited, so the scheduler is
     * guaranteed to switch to this task at least once post-exit. */
    for (volatile int i = 0; i < 10; i++) {
        sys_sleep(2);
    }
    write_str("[threadexit worker done]\n");
    sys_exit(0);   /* thread_on_exit finishes the deferred process teardown */
}

int main(void) {
    char *stack = (char *)sys_mmap(4);   /* 16 KB worker stack */
    if (!stack) {
        write_str("[threadexit] mmap failed\n");
        return 1;
    }
    if (sys_thread_create(worker, stack + 4 * 4096) < 0) {
        write_str("[threadexit] create failed\n");
        return 1;
    }

    write_str("[threadexit main done]\n");
    return 0;   /* deliberately exits without sys_thread_join() */
}
