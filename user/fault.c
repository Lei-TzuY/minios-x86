#include "user_syscall.h"

int main(void) {
    static const char message[] = "Triggering an intentional user page fault.\n";
    volatile int *kernel_address = (volatile int *)0x200000;

    sys_write(message, sizeof(message) - 1);
    return *kernel_address;
}
