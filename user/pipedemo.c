#include "user_syscall.h"

/* User-space pipe + descriptor-duplication demo.  Before exercising dup2 for
 * stdout redirection, prove the ordinary dup syscall works through the real
 * ring-3 int $0x80 path and owns its duplicated file reference correctly. */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

static int test_dup(void) {
    static const char *path = "dupref.tmp";
    int fd = sys_create(path);
    int copy;

    if (fd != 3) return -1;
    if (sys_write_file(fd, "x", 1) != 1) return -1;

    copy = sys_dup(fd);
    if (copy != 4) return -1;
    if (sys_seek(copy, 0, SEEK_CUR) != 1) return -1;

    /* Closing the source must not release the duplicate's VFS reference. */
    if (sys_close(fd) != 0) return -1;
    if (sys_unlink(path) != -1) return -1;

    if (sys_close(copy) != 0) return -1;
    if (sys_unlink(path) != 0) return -1;
    return 0;
}

int main(void) {
    int fds[2];

    if (test_dup() != 0) {
        write_str("[dup failed]\n");
        return 1;
    }

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
