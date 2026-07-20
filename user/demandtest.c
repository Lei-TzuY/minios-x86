#include "user_syscall.h"

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

/* A large local array forces the stack well past the eagerly-mapped top pages,
 * so touching it exercises the demand-paging fault path. */
int main(void) {
    volatile unsigned char buf[12000];
    int ok = 1;

    for (int i = 0; i < 12000; i += 512)
        buf[i] = (unsigned char)(i & 0xFF);
    for (int i = 0; i < 12000; i += 512)
        if (buf[i] != (unsigned char)(i & 0xFF)) ok = 0;

    write_str(ok ? "[demand paging ok]\n" : "[demand paging failed]\n");
    return 0;
}
