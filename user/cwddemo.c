#include "user_syscall.h"

/* Working-directory demo: a user program changes its own cwd, reads it back,
 * and shows that relative paths resolve against the new directory. */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

static void show_cwd(const char *label) {
    char cwd[128];
    sys_getcwd(cwd, sizeof(cwd));
    write_str(label);
    write_str(cwd);
    write_str("]\n");
}

int main(void) {
    show_cwd("[cwd1=");           /* inherited from the shell: / */

    sys_chdir("/fat");
    show_cwd("[cwd2=");           /* /fat */

    /* A relative open now resolves against /fat. */
    int fd = sys_open("hello.txt");
    write_str(fd >= 0 ? "[relopen ok]\n" : "[relopen fail]\n");
    if (fd >= 0) sys_close(fd);

    sys_chdir("..");
    show_cwd("[cwd3=");           /* back to / */

    if (sys_chdir("/nope") != 0)
        write_str("[chdir rejects missing dir]\n");

    return 0;
}
