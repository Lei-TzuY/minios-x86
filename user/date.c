#include "user_syscall.h"

/*
 * date - print the wall-clock date and time from the CMOS RTC (SYS_TIME),
 * formatted as "YYYY-MM-DD HH:MM:SS".
 */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

/* Print `v` as a zero-padded field of `width` digits. */
static void write_pad(int v, int width) {
    char buf[8];
    int i = width;
    buf[i] = '\0';
    while (i > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    sys_write(buf, width);
}

int main(void) {
    struct utime t;
    if (sys_time(&t) != 0) {
        write_str("date: cannot read clock\n");
        return 1;
    }

    write_pad(t.year, 4);
    write_str("-");
    write_pad(t.month, 2);
    write_str("-");
    write_pad(t.day, 2);
    write_str(" ");
    write_pad(t.hour, 2);
    write_str(":");
    write_pad(t.minute, 2);
    write_str(":");
    write_pad(t.second, 2);
    write_str("\n");
    return 0;
}
