#include "pipe.h"
#include "heap.h"
#include "task.h"

/*
 * Wait channels (compared by address):
 *   &p->count     - "data available": readers wait here; writers/closers signal.
 *   &p->read_pos  - "space available": writers wait here; readers/closers signal.
 */

static uint32_t save_irq_disable(void) {
    uint32_t flags;
    __asm__ volatile("pushf; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void restore_irq(uint32_t flags) {
    if (flags & (1 << 9)) {
        __asm__ volatile("sti" ::: "memory");
    }
}

pipe_t *pipe_create(void) {
    pipe_t *p = (pipe_t *)kmalloc(sizeof(pipe_t));

    if (!p) return NULL;
    p->read_pos = 0;
    p->write_pos = 0;
    p->count = 0;
    p->readers = 1;
    p->writers = 1;
    return p;
}

static void pipe_free_if_done(pipe_t *p) {
    if (p->readers == 0 && p->writers == 0) {
        kfree(p);
    }
}

uint32_t pipe_read(pipe_t *p, uint8_t *buffer, uint32_t len) {
    uint32_t flags = save_irq_disable();
    uint32_t got = 0;

    if (!p || !buffer || len == 0) {
        restore_irq(flags);
        return 0;
    }

    /* Block until there is data, or every writer has closed (EOF). */
    while (p->count == 0 && p->writers > 0) {
        task_block_current(&p->count);
    }

    while (got < len && p->count > 0) {
        buffer[got++] = p->buf[p->read_pos];
        p->read_pos = (p->read_pos + 1) % PIPE_BUF_SIZE;
        p->count--;
    }

    if (got > 0) task_wake_all(&p->read_pos);  /* writers: space freed */

    restore_irq(flags);
    return got;  /* 0 means EOF */
}

uint32_t pipe_write(pipe_t *p, const uint8_t *buffer, uint32_t len) {
    uint32_t flags = save_irq_disable();
    uint32_t put = 0;

    if (!p || !buffer || len == 0) {
        restore_irq(flags);
        return 0;
    }

    while (put < len) {
        if (p->readers == 0) break;           /* broken pipe: nobody reading */

        if (p->count == PIPE_BUF_SIZE) {       /* full: let readers drain */
            task_wake_all(&p->count);
            task_block_current(&p->read_pos);
            continue;
        }

        while (put < len && p->count < PIPE_BUF_SIZE) {
            p->buf[p->write_pos] = buffer[put++];
            p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
            p->count++;
        }
        task_wake_all(&p->count);              /* readers: data available */
    }

    restore_irq(flags);
    return put;
}

void pipe_ref_read(pipe_t *p) {
    uint32_t flags = save_irq_disable();
    if (p) p->readers++;
    restore_irq(flags);
}

void pipe_ref_write(pipe_t *p) {
    uint32_t flags = save_irq_disable();
    if (p) p->writers++;
    restore_irq(flags);
}

void pipe_close_read(pipe_t *p) {
    uint32_t flags = save_irq_disable();

    if (!p) { restore_irq(flags); return; }
    if (p->readers > 0) p->readers--;
    if (p->readers == 0) task_wake_all(&p->read_pos);  /* writers: broken pipe */
    pipe_free_if_done(p);
    restore_irq(flags);
}

void pipe_close_write(pipe_t *p) {
    uint32_t flags = save_irq_disable();

    if (!p) { restore_irq(flags); return; }
    if (p->writers > 0) p->writers--;
    if (p->writers == 0) task_wake_all(&p->count);     /* readers: EOF */
    pipe_free_if_done(p);
    restore_irq(flags);
}
