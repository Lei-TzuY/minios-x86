#ifndef IRQ_H
#define IRQ_H

#include <stdint.h>

/*
 * The kernel's mutual-exclusion primitive. On this single-core, non-preemptible
 * design the only thing that can interrupt a critical section is a hardware
 * interrupt (which may run the scheduler), so disabling interrupts is enough to
 * make a section atomic.
 *
 *   uint32_t f = save_irq_disable();   // remember IF, then clear it
 *   ... critical section ...
 *   restore_irq(f);                    // re-enable only if it had been on
 *
 * This exact pair used to be copy-pasted into seven source files; it lives here
 * now so there is one definition to reason about. They are `static inline`, so
 * each translation unit still gets its own copy with no link-time symbol and no
 * change to the inlined codegen versus the old per-file statics.
 *
 * Under HOSTED_TEST (the native unit tests, which run in ring 3 where cli/sti
 * fault and where there are no interrupts to guard against) both compile to
 * no-ops. The kernel build never defines HOSTED_TEST, and the `= 0` init is
 * dead there because the asm output operand overwrites it, so the kernel's
 * generated code is identical to before.
 */
static inline uint32_t save_irq_disable(void) {
    uint32_t flags = 0;
#ifndef HOSTED_TEST
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) :: "memory");
#endif
    return flags;
}

static inline void restore_irq(uint32_t flags) {
#ifndef HOSTED_TEST
    if (flags & (1 << 9)) {
        __asm__ volatile("sti" ::: "memory");
    }
#else
    (void)flags;
#endif
}

#endif /* IRQ_H */
