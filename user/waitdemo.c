#include "user_syscall.h"

/* waitpid demo: fork two children with different exit codes, then reap whichever
 * finishes first using non-blocking waitpid(-1, WNOHANG). */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

static void work(int rounds) {
    for (int r = 0; r < rounds; r++)
        for (volatile int i = 0; i < 100000; i++) { }
}

int main(void) {
    int pid1 = sys_fork();
    if (pid1 == 0) { work(2); sys_exit(7); }

    int pid2 = sys_fork();
    if (pid2 == 0) { work(5); sys_exit(9); }

    int reaped = 0, status;
    while (reaped < 2) {
        int pid = sys_waitpid(-1, &status, WNOHANG);
        if (pid > 0) {
            write_str("[reaped status=");
            write_int(WEXITSTATUS(status));
            write_str("]\n");
            reaped++;
        } else {
            for (volatile int i = 0; i < 50000; i++) { }
        }
    }

    /* No children remain: a wait now reports "no child". */
    if (sys_waitpid(-1, &status, 0) == -1)
        write_str("[no more children]\n");

    return 0;
}
