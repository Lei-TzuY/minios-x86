#include "user_syscall.h"

/*
 * forkredir - checks that fork() inherits the standard streams.
 *
 * The kernel duplicates the open-file table across fork, but fd 0/1 live in
 * separate process fields and used to be left at their defaults, so a child of
 * a process whose stdout was redirected wrote to the terminal instead of the
 * redirect target.
 *
 * Layout: main never redirects (so it can still report), it forks child A,
 * which points its stdout at a file and then forks child B. B writes with no
 * redirection of its own; the bytes must land in the file. main then reads the
 * file back to confirm.
 *
 * Removing the file at the end also proves the inherited references were
 * released: a leaked reference would make unlink fail and leave the file
 * behind, which the suite's RAMFS node count would catch.
 */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

#define MSG     "childout"
#define MSG_LEN 8

int main(void) {
    char buf[16];
    int fd, a, n, ok;

    fd = sys_create("fr.tmp");
    if (fd < 0) { write_str("[forkredir] create failed\n"); return 1; }
    sys_close(fd);

    a = sys_fork();
    if (a < 0) { write_str("[forkredir] fork failed\n"); return 1; }
    if (a == 0) {
        int f = sys_open("fr.tmp");
        if (f < 0) sys_exit(1);
        sys_dup2(f, 1);          /* child A: stdout is now the file */
        sys_close(f);

        int b = sys_fork();      /* child B must inherit that redirection */
        if (b == 0) { sys_write(MSG, MSG_LEN); sys_exit(0); }
        if (b > 0) sys_wait(b);
        sys_exit(0);
    }
    sys_wait(a);

    fd = sys_open("fr.tmp");
    if (fd < 0) { write_str("[forkredir] reopen failed\n"); return 1; }
    n = sys_read_file(fd, buf, MSG_LEN);
    sys_close(fd);

    ok = (n == MSG_LEN);
    for (int i = 0; ok && i < MSG_LEN; i++)
        if (buf[i] != MSG[i]) ok = 0;

    sys_unlink("fr.tmp");
    write_str(ok ? "[forkredir inherited]\n" : "[forkredir NOT inherited]\n");
    write_str("[forkredir done]\n");
    return 0;
}
