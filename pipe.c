#include "pipe.h"
#include "heap.h"
#include "irq.h"
#include "task.h"

/*
 * Wait channels (compared by address):
 *   &p->count     - "data available": readers wait here; writers/closers signal.
 *   &p->read_pos  - "space available": writers wait here; readers/closers signal.
 */

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

/* task_block_killable() cannot be used while an operation owns a lifetime
 * reference: its kill path never returns. Release that reference first so a
 * killed blocked task cannot leak a pipe whose descriptors have all closed. */
static void pipe_block_killable(pipe_t *p, const void *wait_channel,
                                int reading) {
    if (task_kill_pending()) {
        if (reading) pipe_close_read(p);
        else         pipe_close_write(p);
        task_exit(TASK_KILL_STATUS);
    }
    task_block_current(wait_channel);
    if (task_kill_pending()) {
        if (reading) pipe_close_read(p);
        else         pipe_close_write(p);
        task_exit(TASK_KILL_STATUS);
    }
}

uint32_t pipe_read(pipe_t *p, uint8_t *buffer, uint32_t len) {
    uint32_t flags = save_irq_disable();
    uint32_t got = 0;

    if (!p || !buffer || len == 0) {
        restore_irq(flags);
        return 0;
    }
    /* Keep the read end alive across a block. Another thread may close the
     * shared descriptor, but a syscall already using it remains a reader. */
    p->readers++;

    /* Block until there is data, or every writer has closed (EOF). */
    while (p->count == 0 && p->writers > 0) {
        pipe_block_killable(p, &p->count, 1);
    }

    while (got < len && p->count > 0) {
        buffer[got++] = p->buf[p->read_pos];
        p->read_pos = (p->read_pos + 1) % PIPE_BUF_SIZE;
        p->count--;
    }

    if (got > 0) task_wake_all(&p->read_pos);  /* writers: space freed */

    pipe_close_read(p);
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
    /* Symmetric with reads: an in-flight write remains a live writer even if
     * another thread closes the descriptor while this task is blocked. */
    p->writers++;

    while (put < len) {
        if (p->readers == 0) break;           /* broken pipe: nobody reading */

        if (p->count == PIPE_BUF_SIZE) {       /* full: let readers drain */
            task_wake_all(&p->count);
            pipe_block_killable(p, &p->read_pos, 0);
            continue;
        }

        while (put < len && p->count < PIPE_BUF_SIZE) {
            p->buf[p->write_pos] = buffer[put++];
            p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
            p->count++;
        }
        task_wake_all(&p->count);              /* readers: data available */
    }

    pipe_close_write(p);
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
