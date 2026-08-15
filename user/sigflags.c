#include "user_syscall.h"

/*
 * sigflags - regression test for F25: SYS_SIGRETURN must not let a program
 * choose its own EFLAGS.
 *
 * sys_sigreturn restores the saved context from the user stack, and one of the
 * saved fields is EFLAGS. The iret that returns to ring 3 loads it into the
 * real register -- and an iret executed at CPL 0, which is where the kernel is,
 * takes the IOPL field from the stack image rather than preserving it. So
 * whatever a program writes into that field becomes its actual privilege
 * level for port I/O.
 *
 * SYS_SIGRETURN is reachable by any program issuing int $0x80 directly; it does
 * not have to be inside a handler. The frame it names lives in the program's
 * own stack, so the bounds check added for F14 is satisfied -- that check
 * validates the ADDRESS, and this is about the CONTENTS.
 *
 * With IOPL=3 a ring-3 program can drive the ATA ports behind the filesystem's
 * back, and can execute cli: no timer, no scheduler, no keyboard, the whole
 * machine stopped. That puts it in the same class as F2, F14 and F22.
 *
 * The test asks for IOPL=3 and NT and reports what it actually got. Reading
 * EFLAGS back with pushf is non-destructive -- it does not touch a port or
 * disable anything -- so this is safe to run whichever way the kernel behaves.
 */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

/* The context sys_sigreturn expects, at esp + 4. */
struct sigctx {
    unsigned eip, eflags;
    unsigned eax, ebx, ecx, edx, esi, edi, ebp;
    unsigned useresp;
};

#define EFL_IOPL 0x00003000u
#define EFL_NT   0x00004000u
#define EFL_IF   0x00000200u

int main(void) {
    /* On the stack, because sigreturn requires the frame to be in the stack
     * region -- a static would live in .bss and be refused for the address,
     * which is not the property under test. */
    volatile struct { unsigned signum_slot; struct sigctx sc; } frame;
    unsigned flags_after = 0;
    unsigned iopl, nt;

    frame.sc.eax = 0;
    frame.sc.ebx = 0;
    frame.sc.ecx = 0;
    frame.sc.edx = 0;
    frame.sc.esi = 0;
    frame.sc.edi = 0;
    frame.sc.ebp = 0;
    /* Everything a program would want and must not be given. */
    frame.sc.eflags = EFL_IOPL | EFL_NT | EFL_IF | 0x02;

    write_str("[sigflags arming]\n");

    __asm__ volatile(
        "movl %%esp, %[resume_esp]\n\t"   /* come back to where we are now */
        "movl $1f, %[resume_eip]\n\t"     /* and to the label just below */
        "movl %[framep], %%esp\n\t"       /* esp -> [pad][context] */
        "movl $24, %%eax\n\t"             /* SYS_SIGRETURN */
        "int $0x80\n\t"
        "1:\n\t"
        "pushfl\n\t"
        "popl %[flags]\n\t"
        : [resume_esp] "=m"(frame.sc.useresp),
          [resume_eip] "=m"(frame.sc.eip),
          [flags] "=r"(flags_after)
        : [framep] "r"(&frame)
        : "eax", "memory");

    iopl = flags_after & EFL_IOPL;
    nt = flags_after & EFL_NT;

    if (iopl == 0) write_str("[sigflags iopl clear]\n");
    else           write_str("[sigflags ESCALATED iopl]\n");

    if (nt == 0) write_str("[sigflags nt clear]\n");
    else         write_str("[sigflags ESCALATED nt]\n");

    /* Interrupts must be on: a program that came back with IF clear would have
     * stopped the machine, so reaching this line at all says something. */
    if (flags_after & EFL_IF) write_str("[sigflags if set]\n");
    else                      write_str("[sigflags ESCALATED if]\n");

    write_str("[sigflags done]\n");
    return 0;
}
