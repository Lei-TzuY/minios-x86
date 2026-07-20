#include "user_syscall.h"

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

int main(void) {
    write_str("[sleep test started]\n");
    if (sys_sleep(0) != 0 || sys_sleep(-1) != -1) {
        write_str("[sleep argument test failed]\n");
        return 1;
    }
    if (sys_sleep(5) != 0) {
        write_str("[sleep test failed]\n");
        return 1;
    }
    write_str("[sleep test passed]\n");
    return 0;
}
