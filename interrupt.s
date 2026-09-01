.global idt_flush
idt_flush:
    mov 4(%esp), %eax
    lidt (%eax)
    ret

/* Macro for exceptions without error code */
.macro ISR_NOERRCODE num
  .global isr\num
  isr\num:
    cli
    push $0
    push $\num
    jmp isr_common_stub
.endm

/* Macro for exceptions with error code */
.macro ISR_ERRCODE num
  .global isr\num
  isr\num:
    cli
    push $\num
    jmp isr_common_stub
.endm

/* Macro for IRQs */
.macro IRQ num, irq_num
  .global irq\irq_num
  irq\irq_num:
    cli
    push $0
    push $\num
    jmp irq_common_stub
.endm

/* Define ISRs (Exceptions) */
ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8
ISR_NOERRCODE 9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13
ISR_ERRCODE   14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_NOERRCODE 17
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_NOERRCODE 21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_NOERRCODE 30
ISR_NOERRCODE 31

/* Syscall ISR */
ISR_NOERRCODE 128

/* Define IRQs */
IRQ 32, 0
IRQ 33, 1
IRQ 34, 2
IRQ 35, 3
IRQ 36, 4
IRQ 37, 5
IRQ 38, 6
IRQ 39, 7
IRQ 40, 8
IRQ 41, 9
IRQ 42, 10
IRQ 43, 11
IRQ 44, 12
IRQ 45, 13
IRQ 46, 14
IRQ 47, 15

.extern isr_handler
isr_common_stub:
    pusha
    mov %ds, %ax
    push %eax
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    push %esp
    call isr_handler
    pop %eax
    pop %eax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    popa
    add $8, %esp
    sti
    iret

.extern irq_handler
irq_common_stub:
    pusha
    mov %ds, %ax
    push %eax
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    push %esp
    call irq_handler
    pop %ebx
    pop %ebx
    mov %bx, %ds
    mov %bx, %es
    mov %bx, %fs
    mov %bx, %gs
    popa
    add $8, %esp
    sti
    iret

.section .note.GNU-stack,"",@progbits
