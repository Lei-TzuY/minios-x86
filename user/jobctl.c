#include "user_syscall.h"

/*
 * jobctl - demonstrate job control with SIGSTOP / SIGCONT.
 *
 * The child does staged work (a few sleeps, then a "finished" banner). The
 * parent stops the child almost immediately, waits out a window far longer than
 * the child's total work, then continues and reaps it.
 *
 * The proof that the stop took effect is ordering: the child can only print its
 * "finished" banner AFTER the parent's "window elapsed" line, because it made no
 * progress while suspended. And SIGCONT is proven to resume the task by the fact
 * that the program completes at all -- a child left stopped would never exit and
 * the parent's waitpid would block forever (the test would time out).
 */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

int main(void) {
    int pid = sys_fork();
    if (pid < 0) {
        write_str("[jobctl] fork failed\n");
        return 1;
    }

    if (pid == 0) {
        /* Child: staged work that takes ~9 ticks of sleeping in total. */
        write_str("[jc child] start\n");
        for (int i = 0; i < 3; i++) {
            sys_sleep(3);
            write_str("[jc child] work\n");
        }
        write_str("[jc child] finished\n");
        sys_exit(0);
    }

    /* Parent: stop the child before it can finish its work. */
    sys_sleep(2);
    sys_kill(pid, SIGSTOP);
    write_str("[jc parent] sent SIGSTOP\n");

    /* Wait far longer than the child's work budget; if the stop worked the
     * child stays frozen and prints nothing during this window. */
    sys_sleep(20);
    write_str("[jc parent] window elapsed, sending SIGCONT\n");
    sys_kill(pid, SIGCONT);

    int status;
    sys_waitpid(pid, &status, 0);
    write_str("[jc parent] child reaped\n");
    return 0;
}
