#include "user_syscall.h"

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

int main(void) {
    if (sys_spawn("worker") < 0) {
        write_str("[orphan spawn failed]\n");
        return 1;
    }

    write_str("[orphan child launched]\n");
    return 0;
}
