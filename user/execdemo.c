#include "user_syscall.h"

/* fork + exec demo: the parent forks; the child replaces its image with `echo`
 * via execv, so it becomes a different program. The original child code after
 * execv never runs (the image was replaced). */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

int main(void) {
    int pid = sys_fork();

    if (pid < 0) {
        write_str("[fork failed]\n");
        return 1;
    }

    if (pid == 0) {
        const char *argv[] = { "echo", "execok" };
        sys_execv(2, argv);
        write_str("[exec failed]\n");   /* only reached if execv fails */
        sys_exit(1);
    }

    sys_wait(pid);
    write_str("[parent reaped exec child]\n");
    return 0;
}
