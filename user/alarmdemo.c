#include "user_syscall.h"

/* alarm() + SIGALRM demo: arm an alarm, spin in user mode, and let the kernel
 * deliver SIGALRM when the timer expires. */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

static volatile int rang = 0;

static void on_alarm(int signum) {
    (void)signum;
    rang = 1;
}

int main(void) {
    sys_signal(SIGALRM, on_alarm);
    sys_alarm(30);                 /* ~300 ms at 100 Hz */
    write_str("[alarm armed]\n");

    while (!rang) {
        for (volatile int i = 0; i < 100000; i++) { }
    }

    write_str("[alarm fired]\n");
    return 0;
}
