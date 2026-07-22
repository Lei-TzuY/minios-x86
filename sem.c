#include "sem.h"
#include "task.h"

typedef struct {
    int32_t value;
    uint8_t used;
} ksem_t;

static ksem_t semaphores[MAX_SEMAPHORES];

/* See pipe.c for why HOSTED_TEST compiles the privileged cli/sti out to no-ops
 * for the native unit tests; the kernel build never defines it and its codegen
 * is unchanged. */
static uint32_t save_irq_disable(void) {
    uint32_t flags = 0;
#ifndef HOSTED_TEST
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) :: "memory");
#endif
    return flags;
}

static void restore_irq(uint32_t flags) {
#ifndef HOSTED_TEST
    if (flags & (1 << 9)) {
        __asm__ volatile("sti" ::: "memory");
    }
#else
    (void)flags;
#endif
}

static int sem_valid(int id) {
    return id >= 0 && id < MAX_SEMAPHORES;
}

int sem_init(int id, int value) {
    uint32_t flags;

    if (!sem_valid(id) || value < 0) return -1;

    flags = save_irq_disable();
    semaphores[id].value = value;
    semaphores[id].used = 1;
    /* Wake any stale waiters so they re-evaluate against the new count. */
    task_wake_all(&semaphores[id]);
    restore_irq(flags);
    return 0;
}

int sem_wait(int id) {
    uint32_t flags;

    if (!sem_valid(id)) return -1;

    flags = save_irq_disable();
    if (!semaphores[id].used) { restore_irq(flags); return -1; }
    while (semaphores[id].value <= 0) {
        task_block_current(&semaphores[id]);
    }
    semaphores[id].value--;
    restore_irq(flags);
    return 0;
}

int sem_post(int id) {
    uint32_t flags;

    if (!sem_valid(id)) return -1;

    flags = save_irq_disable();
    if (!semaphores[id].used) { restore_irq(flags); return -1; }
    semaphores[id].value++;
    task_wake_one(&semaphores[id]);
    restore_irq(flags);
    return 0;
}
