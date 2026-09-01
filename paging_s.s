.global enable_paging

enable_paging:
    push %ebp
    mov %esp, %ebp

    /* The parameter (directory_address) is at 8(%ebp) */
    mov 8(%ebp), %eax

    /* Load page directory into CR3 */
    mov %eax, %cr3

    /* Enable paging (PG, bit 31) and write-protect (WP, bit 16) so that even
     * supervisor writes honour the read-only bit. WP is required for
     * copy-on-write: a kernel write to a COW page must trap to the fault
     * handler instead of silently modifying the shared frame. */
    mov %cr0, %eax
    or $0x80010000, %eax
    mov %eax, %cr0

    pop %ebp
    ret

.section .note.GNU-stack,"",@progbits
