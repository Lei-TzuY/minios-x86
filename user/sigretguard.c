#include "user_syscall.h"

/*
 * sigretguard - regression test for a fixed user-triggerable kernel halt.
 *
 * SYS_SIGRETURN restores a saved register context from the user stack. It used
 * to dereference regs->useresp with no validation at all, and it is reachable
 * by any program issuing int $0x80 directly -- being inside a signal handler
 * was never required. Pointing the stack somewhere unmapped therefore made the
 * kernel read from that address in ring 0, and the page-fault handler answers a
 * kernel-mode fault by halting the whole machine.
 *
 * This program deliberately performs that attack: it drops ESP far below the
 * user stack and invokes SYS_SIGRETURN. (Taking an interrupt from ring 3
 * switches to the kernel stack via the TSS, so the bogus ESP does not stop the
 * int itself.) The kernel must reject the frame and kill only this process, so
 * the marker below is never reached and the rest of the suite keeps running --
 * before the fix the log simply stopped here.
 */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

int main(void) {
    write_str("[sigretguard arming]\n");

    __asm__ volatile(
        "movl $0x1000, %%esp\n\t"   /* far below USER_STACK_BOTTOM, unmapped */
        "movl $24, %%eax\n\t"       /* SYS_SIGRETURN */
        "int $0x80\n\t"
        ::: "eax", "memory");

    /* Only reached if the kernel accepted the bogus frame. */
    write_str("[sigretguard SURVIVED]\n");
    return 0;
}
