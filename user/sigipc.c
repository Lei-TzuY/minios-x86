#include "user_syscall.h"

/* Inter-process signal demo. Run with no args it acts as the parent: it spawns
 * a copy of itself as the child, waits for the child to install a SIGUSR1
 * handler, then signals it with sys_kill. The child catches SIGUSR1 and exits. */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

static volatile int caught = 0;

static void on_usr1(int signum) {
    (void)signum;
    write_str("[child caught SIGUSR1]\n");
    caught = 1;
}

int main(int argc, char **argv) {
    (void)argv;

    if (argc > 1) {
        /* Child: install handler, announce readiness, spin until signalled. */
        sys_signal(SIGUSR1, on_usr1);
        write_str("[child ready]\n");
        while (!caught) {
            for (volatile int i = 0; i < 200000; i++) { }
        }
        write_str("[child exiting]\n");
        return 0;
    }

    /* Parent: spawn the child, let it get ready, then signal it. */
    const char *child_argv[] = { "sigipc", "child" };
    int pid = sys_spawn_argv(2, child_argv);
    if (pid < 0) {
        write_str("[sigipc spawn failed]\n");
        return 1;
    }

    sys_sleep(30);                 /* ~300ms: child installs handler & spins */
    if (sys_kill(pid, SIGUSR1) != 0) {
        write_str("[sigipc kill failed]\n");
        return 1;
    }
    sys_wait(pid);
    write_str("[parent done]\n");
    return 0;
}
