#include "user_syscall.h"

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

static void write_dec(int value) {
    char buffer[12];
    int index = sizeof(buffer);

    do {
        buffer[--index] = (char)('0' + value % 10);
        value /= 10;
    } while (value > 0);
    sys_write(buffer + index, sizeof(buffer) - index);
}

int main(void) {
    write_str("[worker pid=");
    write_dec(sys_getpid());
    write_str(" started]\n");

    for (int i = 0; i < 3; i++) {
        write_str("[worker step]\n");
        sys_yield();
    }

    write_str("[worker done]\n");
    return 0;
}
