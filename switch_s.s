.global switch_task

/* void switch_task(uint32_t *old_esp, uint32_t new_esp) */
switch_task:
    /* push current context */
    push %ebp
    push %ebx
    push %esi
    push %edi

    /* old_esp is at 20(%esp), new_esp is at 24(%esp) */
    mov 20(%esp), %eax  /* eax = pointer to old_esp */
    mov %esp, (%eax)    /* save current esp to *old_esp */

    mov 24(%esp), %esp  /* load new_esp into esp */

    /* pop new context */
    pop %edi
    pop %esi
    pop %ebx
    pop %ebp

    ret

.section .note.GNU-stack,"",@progbits
