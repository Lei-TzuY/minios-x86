#include "user_syscall.h"

/*
 * uptime - read the kernel's monotonic clock (SYS_UPTIME) from ring 3.
 *
 * Prints the current uptime, sleeps a known number of ticks, then reads the
 * clock again. The elapsed count must be at least the sleep duration -- a
 * deterministic check that does not depend on the absolute tick value.
 */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

#define SLEEP_TICKS 50   /* 0.5 s at the 100 Hz PIT */

int main(void) {
    int t0 = sys_uptime();
    write_str("[uptime] up ");
    write_int(t0 / 100);
    write_str("s (ticks=");
    write_int(t0);
    write_str(")\n");

    sys_sleep(SLEEP_TICKS);

    int delta = sys_uptime() - t0;
    write_str("[uptime] elapsed ticks=");
    write_int(delta);
    write_str("\n");
    write_str(delta >= SLEEP_TICKS ? "[uptime ok]\n" : "[uptime FAIL]\n");
    return 0;
}
