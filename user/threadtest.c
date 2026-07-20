#include "user_syscall.h"

/*
 * threadtest - two threads sharing one address space, coordinated by a
 * semaphore mutex.
 *
 * Both threads increment the SAME global `counter` (they share all memory, no
 * copy-on-write), each 200 times, under a binary semaphore. The only way the
 * final count can be exactly 400 is if (a) the threads really share `counter`
 * and (b) the mutex serialises the read-modify-write so no updates are lost.
 * Their stacks come from sys_mmap so they don't clobber each other or main.
 */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

#define SEM_MUTEX 0
#define ITERS     200
#define STACK_PAGES 4          /* 16 KB per thread */

static volatile int counter = 0;

static void worker(void) {
    for (int i = 0; i < ITERS; i++) {
        sys_sem_wait(SEM_MUTEX);
        counter++;                 /* shared read-modify-write */
        sys_sem_post(SEM_MUTEX);
    }
    sys_exit(0);                   /* thread_on_exit drops the thread count */
}

int main(void) {
    sys_sem_init(SEM_MUTEX, 1);

    char *s1 = (char *)sys_mmap(STACK_PAGES);
    char *s2 = (char *)sys_mmap(STACK_PAGES);
    if (!s1 || !s2) { write_str("[thread] mmap failed\n"); return 1; }

    /* Stacks grow down, so hand each thread the TOP of its region. */
    if (sys_thread_create(worker, s1 + STACK_PAGES * 4096) < 0 ||
        sys_thread_create(worker, s2 + STACK_PAGES * 4096) < 0) {
        write_str("[thread] create failed\n");
        return 1;
    }

    sys_thread_join();             /* wait for both threads */

    write_str("[thread] counter=");
    write_int(counter);
    write_str("\n");
    write_str(counter == 2 * ITERS ? "[thread ok]\n" : "[thread FAIL]\n");
    return 0;
}
