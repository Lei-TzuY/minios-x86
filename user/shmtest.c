#include "user_syscall.h"

/*
 * shmtest - prove that SYS_SHM gives fork'd processes a genuinely shared,
 * writable page (not a copy-on-write copy).
 *
 * The parent writes 100 before forking. The child must SEE that 100 (the page
 * is inherited), then writes 123. After the child exits the parent reads the
 * page again: under copy-on-write it would still read 100, but with real shared
 * memory it reads 123 -- so "final=123" is the deterministic proof of sharing.
 */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

int main(void) {
    volatile int *shm = (volatile int *)sys_shm();
    if (!shm) {
        write_str("[shm] alloc failed\n");
        return 1;
    }

    *shm = 100;

    int pid = sys_fork();
    if (pid < 0) {
        write_str("[shm] fork failed\n");
        return 1;
    }

    if (pid == 0) {
        write_str("[shm child] saw=");
        write_int(*shm);        /* should be 100, written by the parent */
        write_str("\n");
        *shm = 123;             /* hand a new value back to the parent */
        sys_exit(0);
    }

    sys_wait(pid);
    write_str("[shm parent] final=");
    write_int(*shm);            /* 123 if the page is truly shared */
    write_str("\n");
    return 0;
}
