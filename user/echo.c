#include "user_syscall.h"

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) sys_write(" ", 1);
        write_str(argv[i]);
    }
    sys_write("\n", 1);
    return 0;
}
