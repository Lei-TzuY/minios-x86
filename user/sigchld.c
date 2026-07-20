#include "user_syscall.h"

/* SIGCHLD demo: the parent installs a SIGCHLD handler, forks a child that does
 * a little work and exits, then spins until the kernel delivers SIGCHLD. */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

static volatile int chld_count = 0;

static void on_sigchld(int signum) {
    (void)signum;
    chld_count++;
}

int main(void) {
    int pid;

    sys_signal(SIGCHLD, on_sigchld);

    pid = sys_fork();
    if (pid < 0) {
        write_str("[fork failed]\n");
        return 1;
    }

    if (pid == 0) {
        /* Child: brief work, then exit (kernel posts SIGCHLD to the parent). */
        for (volatile int i = 0; i < 800000; i++) { }
        sys_exit(0);
    }

    /* Parent: run until the SIGCHLD handler observes the child's exit. */
    while (chld_count == 0) {
        for (volatile int i = 0; i < 100000; i++) { }
    }
    write_str("[parent got SIGCHLD]\n");

    sys_wait(pid);
    write_str("[child reaped]\n");
    return 0;
}
