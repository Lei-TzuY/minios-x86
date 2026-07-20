#include "user_syscall.h"

/*
 * cputime - demonstrate per-process CPU accounting (SYS_CPUTIME).
 *
 * Burns CPU until the kernel reports at least TARGET ticks charged to this
 * process, then prints the total. This is deterministic: a working accounting
 * path always reaches the target and stops early; a broken one (stuck at 0)
 * exhausts the safety cap and reports failure rather than looping forever.
 */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

static void print_uint(unsigned v) {
    char buf[12];
    int n = 0;
    if (v == 0) { sys_write("0", 1); return; }
    while (v) { buf[n++] = (char)('0' + v % 10); v /= 10; }
    char out[12];
    for (int i = 0; i < n; i++) out[i] = buf[n - 1 - i];
    sys_write(out, n);
}

#define TARGET 3
#define MAX_ROUNDS 2000000

int main(void) {
    volatile unsigned sink = 0;
    int rounds = 0;

    while (sys_cputime() < TARGET && rounds < MAX_ROUNDS) {
        for (volatile int i = 0; i < 20000; i++) sink += (unsigned)i;
        rounds++;
    }

    int ticks = sys_cputime();
    write_str("[cputime] ticks=");
    print_uint((unsigned)ticks);
    write_str("\n");
    write_str(ticks >= TARGET ? "[cputime ok]\n" : "[cputime FAIL]\n");
    return 0;
}
