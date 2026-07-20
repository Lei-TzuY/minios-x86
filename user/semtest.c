#include "user_syscall.h"

/*
 * semtest - a classic single-slot producer/consumer over shared memory, proving
 * the counting-semaphore syscalls block and wake correctly.
 *
 * The parent (producer) hands items 1..5 through one shared int, guarded by two
 * semaphores: EMPTY (slots free, init 1) and FULL (items ready, init 0). The
 * child (consumer) sums what it receives. Correct synchronisation is the only
 * way the sum can be exactly 1+2+3+4+5 = 15: a missing block would let the
 * consumer read stale/duplicate items, and a missing wake would deadlock (the
 * test would then time out).
 */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

#define SEM_EMPTY 0
#define SEM_FULL  1
#define ITEMS     5

int main(void) {
    volatile int *slot = (volatile int *)sys_shm();
    if (!slot) { write_str("[semtest] shm failed\n"); return 1; }

    sys_sem_init(SEM_EMPTY, 1);   /* one free slot to begin */
    sys_sem_init(SEM_FULL, 0);    /* nothing produced yet */

    int pid = sys_fork();
    if (pid < 0) { write_str("[semtest] fork failed\n"); return 1; }

    if (pid == 0) {
        int sum = 0;
        for (int i = 0; i < ITEMS; i++) {
            sys_sem_wait(SEM_FULL);   /* wait for an item */
            sum += *slot;
            sys_sem_post(SEM_EMPTY);  /* slot is free again */
        }
        write_str("[semtest] sum=");
        write_int(sum);
        write_str("\n");
        write_str(sum == 15 ? "[semtest ok]\n" : "[semtest FAIL]\n");
        sys_exit(0);
    }

    for (int i = 0; i < ITEMS; i++) {
        sys_sem_wait(SEM_EMPTY);      /* wait for a free slot */
        *slot = i + 1;                /* produce item 1..5 */
        sys_sem_post(SEM_FULL);       /* signal an item is ready */
    }

    sys_wait(pid);
    write_str("[semtest] producer done\n");
    return 0;
}
