#include "user_syscall.h"

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

/* A global the child will modify in its own copy of the address space. */
static int shared = 100;

int main(void) {
    int pid = sys_fork();

    if (pid < 0) {
        write_str("[fork failed]\n");
        return 1;
    }

    if (pid == 0) {
        /* Child: change our copy of `shared`, then exit. */
        shared = 200;
        write_str("[child] shared=");
        write_int(shared);
        write_str("\n");
        sys_exit(0);
    }

    /* Parent: wait for the child, then show our `shared` is untouched. */
    sys_wait(pid);
    write_str("[parent] shared=");
    write_int(shared);
    write_str("\n");
    write_str("[fork test done]\n");
    return 0;
}
