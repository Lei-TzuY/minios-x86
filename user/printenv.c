#include "user_syscall.h"

/*
 * printenv NAME - print the value of an inherited environment variable.
 *
 * Demonstrates that the environment set by a parent (e.g. ush's `export`)
 * survives both fork and execv: this program is spawned and exec'd by the
 * shell, yet still sees the variable through sys_getenv.
 */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

int main(int argc, char **argv) {
    char value[ENV_VAL_MAX];

    if (argc < 2) {
        write_str("printenv: usage: printenv NAME\n");
        return 1;
    }

    if (sys_getenv(argv[1], value, sizeof(value)) < 0) {
        write_str(argv[1]);
        write_str(" not set\n");
        return 1;
    }

    write_str(argv[1]);
    write_str("=");
    write_str(value);
    write_str("\n");
    return 0;
}
