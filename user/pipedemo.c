#include "user_syscall.h"

/* User-space pipe + dup2 demo (the mechanism a ring-3 shell uses for "cmd1 |
 * cmd2"): the child redirects its stdout onto a pipe with dup2 and writes via
 * the normal stdout path; the parent reads the pipe and echoes it. */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

int main(void) {
    int fds[2];

    if (sys_pipe(fds) != 0) {
        write_str("[pipe failed]\n");
        return 1;
    }

    int pid = sys_fork();
    if (pid < 0) {
        write_str("[fork failed]\n");
        return 1;
    }

    if (pid == 0) {
        /* Child: stdout -> pipe write end, then write the normal way. */
        sys_close(fds[0]);
        sys_dup2(fds[1], 1);
        sys_close(fds[1]);
        sys_write("piped via dup2\n", 15);   /* goes into the pipe */
        sys_exit(0);
    }

    /* Parent: read what the child produced and echo it to the terminal. */
    sys_close(fds[1]);
    char buf[64];
    int n = sys_read_file(fds[0], buf, sizeof(buf));
    if (n > 0) sys_write(buf, n);
    sys_close(fds[0]);

    sys_wait(pid);
    write_str("[pipe demo done]\n");
    return 0;
}
