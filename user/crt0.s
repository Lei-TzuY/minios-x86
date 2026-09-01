/* crt0.s - minimal C runtime for miniOS user programs */
.global _start
.section .text

_start:
    pop  %eax       /* eax = argc; ESP now points to argv[0] */
    mov  %esp, %ecx /* ecx = argv (pointer to first argv[] slot) */
    push %ecx       /* arg2: char **argv */
    push %eax       /* arg1: int argc */
    call main
    mov  %eax, %ebx /* exit code = return value of main() */
    mov  $3,   %eax /* SYS_EXIT */
    int  $0x80
1:  jmp  1b

/* Signal trampoline: a handler returns here, and we ask the kernel to restore
 * the pre-signal context via SYS_SIGRETURN. Uses no stack. */
.global __sig_trampoline
__sig_trampoline:
    mov  $24, %eax  /* SYS_SIGRETURN */
    int  $0x80

.section .note.GNU-stack,"",@progbits
