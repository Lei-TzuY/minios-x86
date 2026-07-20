#include "user_syscall.h"

int main(void) {
    static const char passed[] = "[bad user pointer rejected]\n";
    static const char failed[] = "[bad user pointer accepted]\n";
    const char *unmapped_user_pointer = (const char *)0x3E0000;

    if (sys_write(unmapped_user_pointer, 1) == -1) {
        sys_write(passed, sizeof(passed) - 1);
        return 0;
    }

    sys_write(failed, sizeof(failed) - 1);
    return 1;
}
