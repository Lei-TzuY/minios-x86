#include "../gdt.h"
#include "../idt.h"
#include "../io.h"
#include "../isr.h"

/* A small Multiboot test image linked against the REAL interrupt.o, GDT and
 * IDT. Hosted tests cannot execute these privileged entry/iret instructions.
 * All device IRQs are masked: INT 0x20 deterministically exercises irq0 and
 * its common stub without letting the scheduler or another entry clear DF. */
extern void isr128(void);
extern void irq0(void);

#define DF 0x400u
#define IF 0x200u

static uint32_t expected_vector;
static uint32_t expected_df;
static unsigned entries;
static unsigned failures;

static void report(const char *text) {
    while (*text) outb(0xE9, (uint8_t)*text++);
}

void isr_handler(registers_t *regs) {
    uint32_t live_flags;

    /* Capture BEFORE clearing DF ourselves. CLD here only makes failure
     * reporting safe on the unfixed entry; it cannot repair the observation. */
    __asm__ volatile("pushfl; popl %0; cld"
                     : "=r"(live_flags) :: "memory", "cc");
    entries++;
    if (live_flags & DF) {
        report(expected_vector == 32 ? "[entry irq DF FAIL]\n" :
                                       "[entry isr DF FAIL]\n");
        failures++;
    }
    if ((live_flags & IF) || regs->int_no != expected_vector ||
        regs->err_code != 0 || regs->cs != 0x08 ||
        (regs->eflags & (DF | IF)) != expected_df) {
        report("[entry saved frame FAIL]\n");
        failures++;
    }
}

/* The same observer measures both production stubs at their C ABI boundary. */
void irq_handler(registers_t *regs) __attribute__((alias("isr_handler")));

void kernel_main(uint32_t magic, uint32_t info) {
    uint32_t returned_flags;

    (void)magic;
    (void)info;
    gdt_install();
    idt_install();
    outb(0x21, 0xFF);
    outb(0xA1, 0xFF);
    idt_set_gate(0x80, (uint32_t)isr128, 0x08, 0x8E);
    idt_set_gate(0x20, (uint32_t)irq0, 0x08, 0x8E);

    /* Test both initial DF states and prove iret preserves DF and IF=0.
     * The ring-3 stress suite separately covers int 0x80 and real COW #PFs. */
    for (expected_df = 0; expected_df <= DF; expected_df += DF) {
        expected_vector = 128;
        __asm__ volatile("pushl %1; popfl; int $0x80; pushfl; popl %0; cld"
                         : "=r"(returned_flags)
                         : "r"(expected_df | 2u) : "memory", "cc");
        if ((returned_flags & (DF | IF)) != expected_df) {
            report("[entry isr return FAIL]\n");
            failures++;
        }

        expected_vector = 32;
        __asm__ volatile("pushl %1; popfl; int $0x20; pushfl; popl %0; cld"
                         : "=r"(returned_flags)
                         : "r"(expected_df | 2u) : "memory", "cc");
        if ((returned_flags & (DF | IF)) != expected_df) {
            report("[entry irq return FAIL]\n");
            failures++;
        }
    }
    if (entries != 4) failures++;
    report(failures ? "[interrupt entry FAILED]\n" : "[interrupt entry PASS]\n");
    outb(0xF4, failures ? 0x11 : 0x10);  /* QEMU isa-debug-exit: 35 or 33 */
    while (1) __asm__ volatile("cli; hlt");
}
