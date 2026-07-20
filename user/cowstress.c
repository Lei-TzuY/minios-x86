#include "user_syscall.h"

/* Copy-on-write stress: a multi-page array is shared across fork, then the
 * child rewrites every element. Each touched page must be copied privately, so
 * the parent's data stays intact while the child's diverges. */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

#define N 2000   /* ~8 KB: spans several pages */
static volatile int data[N];

int main(void) {
    for (int i = 0; i < N; i++) data[i] = i;

    int pid = sys_fork();
    if (pid == 0) {
        int ok = 1;
        for (int i = 0; i < N; i++) data[i] = i * 2;   /* triggers COW per page */
        for (int i = 0; i < N; i++) if (data[i] != i * 2) ok = 0;
        write_str(ok ? "[child cow ok]\n" : "[child cow fail]\n");
        sys_exit(0);
    }

    sys_wait(pid);

    int ok = 1;
    for (int i = 0; i < N; i++) if (data[i] != i) ok = 0;   /* parent untouched */
    write_str(ok ? "[parent cow ok]\n" : "[parent cow fail]\n");
    return 0;
}
