#include "user_syscall.h"

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

static volatile int caught = 0;

/* Runs asynchronously when SIGINT (Ctrl+C) is delivered. */
static void on_sigint(int signum) {
    (void)signum;
    write_str("[caught SIGINT]\n");
    caught = 1;
}

int main(void) {
    if (sys_signal(SIGINT, on_sigint) != 0) {
        write_str("[signal install failed]\n");
        return 1;
    }

    write_str("[sigtest waiting]\n");

    /* Spin in user mode so the timer can interrupt us and deliver the signal. */
    while (!caught) {
        for (volatile int i = 0; i < 200000; i++) { }
    }

    write_str("[sigtest exiting]\n");
    return 0;
}
