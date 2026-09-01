.global enter_user_mode

/* void enter_user_mode(uint32_t eip, uint32_t user_esp) */
enter_user_mode:
    cli
    mov 4(%esp), %ecx   /* ECX = target eip */
    mov 8(%esp), %edx   /* EDX = user stack pointer */

    mov $0x23, %ax      /* User Data Segment selector */
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    push $0x23          /* SS  */
    push %edx           /* ESP (user stack) */
    pushf               /* EFLAGS */
    pop %eax
    or $0x200, %eax     /* Enable IF */
    push %eax
    push $0x1B          /* CS (User Code Segment) */
    push %ecx           /* EIP */

    iret                /* Switch to ring 3 */

/* void enter_user_mode_iret(const fork_frame_t *frame)
 * Resume a user context from a saved frame (used by a forked child). The frame
 * layout is: eip, eflags, useresp, eax, ebx, ecx, edx, esi, edi, ebp. */
.global enter_user_mode_iret
enter_user_mode_iret:
    cli
    mov 4(%esp), %ebp   /* EBP = frame pointer */

    mov $0x23, %ax      /* User Data Segment */
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    push $0x23          /* SS */
    push 8(%ebp)        /* useresp */
    mov 4(%ebp), %eax   /* eflags */
    or $0x200, %eax     /* enable IF */
    push %eax
    push $0x1B          /* CS */
    push 0(%ebp)        /* eip */

    mov 16(%ebp), %ebx
    mov 20(%ebp), %ecx
    mov 24(%ebp), %edx
    mov 28(%ebp), %esi
    mov 32(%ebp), %edi
    mov 12(%ebp), %eax  /* eax (0 for the child) */
    mov 36(%ebp), %ebp  /* ebp last: overwrites the frame pointer */

    iret

.section .note.GNU-stack,"",@progbits
