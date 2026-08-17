#include "sem.h"
#include "irq.h"
#include "task.h"

typedef struct {
    int32_t value;
    uint8_t used;
} ksem_t;

static ksem_t semaphores[MAX_SEMAPHORES];

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
        task_block_killable(&semaphores[id]);
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
    if (semaphores[id].value == INT32_MAX) {
        restore_irq(flags);
        return -1;
    }
    semaphores[id].value++;
    task_wake_one(&semaphores[id]);
    restore_irq(flags);
    return 0;
}
