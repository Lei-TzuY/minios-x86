#include "user_syscall.h"

/* pause()/getppid() demo: the parent blocks in pause() until the child signals
 * it. The child finds the parent with getppid() and sends SIGUSR1. */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

static volatile int woke = 0;

static void on_usr1(int signum) {
    (void)signum;
    woke = 1;
}

int main(void) {
    sys_signal(SIGUSR1, on_usr1);

    int pid = sys_fork();
    if (pid < 0) {
        write_str("[fork failed]\n");
        return 1;
    }

    if (pid == 0) {
        /* Child: let the parent reach pause(), then signal it by parent pid. */
        int ppid = sys_getppid();
        for (volatile int i = 0; i < 400000; i++) { }
        sys_kill(ppid, SIGUSR1);
        sys_exit(0);
    }

    write_str("[parent pausing]\n");
    sys_pause();                 /* blocks until SIGUSR1 from the child */
    write_str(woke ? "[parent woken by child]\n" : "[spurious wake]\n");

    sys_wait(pid);
    return 0;
}
