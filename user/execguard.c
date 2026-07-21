#include "user_syscall.h"

/*
 * execguard - regression test for a fixed kernel safety bug: calling execv()
 * while a sibling thread (SYS_THREAD_CREATE) is still alive used to free the
 * shared address space out from under that thread (a use-after-free of the
 * same class as the deferred-teardown bug in process exit).
 *
 * This program creates a worker thread that parks on a semaphore, then calls
 * execv("hello"). The kernel must REFUSE the exec (return -1) because a
 * sibling thread shares the address space; if it wrongly succeeded, the image
 * would be replaced and the worker's address space freed, crashing the OS.
 * After the (expected) rejection, main releases the worker and joins it.
 */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

#define SEM_GUARD   4
#define STACK_PAGES 4          /* 16 KB worker stack */

static void worker(void) {
    sys_sem_wait(SEM_GUARD);   /* block until main releases us */
    write_str("[execguard worker ran]\n");
    sys_exit(0);
}

int main(void) {
    sys_sem_init(SEM_GUARD, 0);

    char *stk = (char *)sys_mmap(STACK_PAGES);
    if (!stk) { write_str("[execguard] mmap failed\n"); return 1; }

    if (sys_thread_create(worker, stk + STACK_PAGES * 4096) < 0) {
        write_str("[execguard] create failed\n");
        return 1;
    }

    /* A sibling thread now shares our address space. execv must refuse rather
     * than tear the address space down while the worker still references it. */
    const char *argv[1] = { "hello" };
    int r = sys_execv(1, argv);
    if (r == -1) write_str("[execguard exec rejected]\n");
    else         write_str("[execguard exec WRONGLY SUCCEEDED]\n");

    sys_sem_post(SEM_GUARD);    /* release the worker so it can exit */
    sys_thread_join();
    write_str("[execguard done]\n");
    return 0;
}
