#include "user_syscall.h"

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

int main(void) {
    int first = sys_spawn("worker");
    int second = sys_spawn("worker");
    int cat_first;
    int cat_second;
    int sleep_first;
    int sleep_second;
    int echo_pid;
    const char *echo_argv[] = { "echo", "hello", "from", "spawn" };

    if (first < 0 || second < 0) {
        write_str("[spawn test failed]\n");
        return 1;
    }

    write_str("[spawned two workers]\n");
    if (sys_wait(first) != 0 || sys_wait(second) != 0) {
        write_str("[wait test failed]\n");
        return 1;
    }

    write_str("[spawn/wait test passed]\n");

    cat_first = sys_spawn("cat");
    cat_second = sys_spawn("cat");
    if (cat_first < 0 || cat_second < 0 ||
        sys_wait(cat_first) != 0 || sys_wait(cat_second) != 0) {
        write_str("[per-process file table test failed]\n");
        return 1;
    }

    write_str("[per-process file table test passed]\n");

    sleep_first = sys_spawn("sleeptest");
    sleep_second = sys_spawn("sleeptest");
    if (sleep_first < 0 || sleep_second < 0 ||
        sys_wait(sleep_first) != 0 || sys_wait(sleep_second) != 0) {
        write_str("[sleep queue test failed]\n");
        return 1;
    }

    write_str("[sleep queue test passed]\n");

    /* Test SYS_SPAWN_ARGV: spawn echo with arguments and wait for it. */
    echo_pid = sys_spawn_argv(4, echo_argv);
    if (echo_pid < 0 || sys_wait(echo_pid) != 0) {
        write_str("[spawn_argv test failed]\n");
        return 1;
    }

    write_str("[spawn_argv test passed]\n");
    return 0;
}
