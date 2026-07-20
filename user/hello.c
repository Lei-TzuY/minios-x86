#include "user_syscall.h"

/* Demo: print a message and exit.
 * Compiled with gcc -m32 -nostdlib, linked at 0x300000 (USER_LOAD_BASE). */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

int main(void) {
    write_str("Hello from user space!\n");
    write_str("This program was compiled by gcc -m32 and runs on our OS.\n");
    write_str("Syscalls (int $0x80) are working correctly.\n");
    return 0;
}
