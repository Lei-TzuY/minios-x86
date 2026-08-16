#include "task.h"
#include "gdt.h"
#include "irq.h"
#include "pmm.h"
#include "vga.h"

volatile task_t *current_task = 0;
volatile task_t *ready_queue = 0;

uint32_t next_pid = 1;
static task_t *retired_task = 0;
static task_t *blocked_tasks = 0;

extern void switch_task(uint32_t *old_esp, uint32_t new_esp);

static void reap_retired_task(void) {
    task_t *task = retired_task;

    if (!task) return;

    retired_task = 0;
    if (task->stack_bottom) {
        pmm_free_block(task->stack_bottom);
    }
    pmm_free_block(task);
}

static void activate_task(task_t *task) {
    current_task = task;
    set_kernel_stack(task->kernel_stack_top);
    paging_activate_address_space(task->address_space);
}

static task_t *find_ready_previous(task_t *task) {
    task_t *previous;

    if (!ready_queue || !task) return 0;

    previous = task;
    while (previous->next != task) {
        previous = previous->next;
        if (previous == task) return 0;
    }
    return previous;
}

static void add_ready_task(task_t *task) {
    task->state = TASK_READY;
    task->wait_channel = 0;
    task->blocked_next = 0;

    if (!ready_queue) {
        task->next = task;
        ready_queue = task;
        return;
    }

    task->next = ready_queue->next;
    ready_queue->next = task;
}

static void idle_task_runner(void) {
    while (1) {
        __asm__ volatile("sti; hlt" ::: "memory");
    }
}

void tasking_init(void) {
    /* Initialize the main kernel thread (the one currently running) */
    if (current_task) return;

    task_t *main_task = (task_t*)pmm_alloc_block();
    if (!main_task) return;

    main_task->id = 0;
    main_task->next = main_task;
    main_task->blocked_next = 0;
    main_task->stack_bottom = 0;
    main_task->kernel_stack_top = 0x90000;
    main_task->address_space = paging_get_kernel_address_space();
    main_task->process = 0;
    main_task->on_exit = 0;
    main_task->wait_channel = 0;
    main_task->state = TASK_READY;
    main_task->kill_pending = 0;

    current_task = main_task;
    ready_queue = main_task;

    /* Keep one runnable kernel task available while every real task waits. */
    create_task(idle_task_runner, 0, paging_get_kernel_address_space(), 0, 0, 0);
}

task_t* create_task(void (*entry)(void), task_exit_callback_t on_exit,
                    address_space_t *address_space, struct process *process,
                    uint32_t user_entry, uint32_t user_stack) {
    uint32_t flags = save_irq_disable();

    if (!entry || !ready_queue || !address_space) {
        restore_irq(flags);
        return 0;
    }

    task_t *new_task = (task_t*)pmm_alloc_block();
    if (!new_task) {
        restore_irq(flags);
        return 0;
    }

    new_task->id = next_pid++;
    new_task->on_exit = on_exit;
    new_task->blocked_next = 0;
    new_task->wait_channel = 0;
    new_task->state = TASK_READY;
    new_task->kill_pending = 0;
    new_task->user_entry = user_entry;
    new_task->user_stack = user_stack;

    /* Allocate stack (4KB) */
    new_task->stack_bottom = pmm_alloc_block();
    if (!new_task->stack_bottom) {
        pmm_free_block(new_task);
        restore_irq(flags);
        return 0;
    }
    uint32_t *stack = (uint32_t*)((uint32_t)new_task->stack_bottom + PMM_BLOCK_SIZE);
    new_task->kernel_stack_top = (uint32_t)stack;
    new_task->address_space = address_space;
    new_task->process = process;
    
    /* Set up the initial stack for context switch */
    *(--stack) = (uint32_t)entry; /* eip */
    *(--stack) = 0; /* ebp */
    *(--stack) = 0; /* ebx */
    *(--stack) = 0; /* esi */
    *(--stack) = 0; /* edi */
    
    new_task->esp = (uint32_t)stack;
    
    /* Add to ready queue (circular linked list) */
    add_ready_task(new_task);
    
    restore_irq(flags);
    return new_task;
}

task_t *task_get_current(void) {
    return (task_t *)current_task;
}

void task_block_current(const void *wait_channel) {
    uint32_t flags = save_irq_disable();
    task_t *task = (task_t *)current_task;
    task_t *previous;
    task_t *next;

    if (!task || task->state != TASK_READY || task->next == task) {
        restore_irq(flags);
        return;
    }

    previous = find_ready_previous(task);
    if (!previous) {
        restore_irq(flags);
        return;
    }

    next = task->next;
    previous->next = next;
    if (ready_queue == task) ready_queue = next;

    task->next = 0;
    task->blocked_next = 0;
    task->wait_channel = wait_channel;
    task->state = TASK_BLOCKED;
    /* Append at the tail so `blocked_tasks` runs oldest-waiter first. Head
     * insertion would make task_wake_one() (which wakes the first match) pick
     * the most recent waiter, i.e. LIFO: with a steady stream of new waiters
     * on the same channel an early waiter could be passed over indefinitely.
     * Appending gives FIFO wakeups, the fair and conventional choice. The walk
     * is O(blocked tasks), which is bounded by the small task count and runs
     * with interrupts already disabled. */
    if (!blocked_tasks) {
        blocked_tasks = task;
    } else {
        task_t *last = blocked_tasks;
        while (last->blocked_next) last = last->blocked_next;
        last->blocked_next = task;
    }

    activate_task(next);
    switch_task(&task->esp, next->esp);
    restore_irq(flags);
}

void task_wake_one(const void *wait_channel) {
    uint32_t flags = save_irq_disable();
    task_t **link = &blocked_tasks;

    while (*link) {
        task_t *task = *link;

        if (task->wait_channel == wait_channel) {
            *link = task->blocked_next;
            add_ready_task(task);
            break;
        }
        link = &task->blocked_next;
    }
    restore_irq(flags);
}

void task_wake_all(const void *wait_channel) {
    uint32_t flags = save_irq_disable();
    task_t **link = &blocked_tasks;

    while (*link) {
        task_t *task = *link;

        if (task->wait_channel == wait_channel) {
            *link = task->blocked_next;
            add_ready_task(task);
        } else {
            link = &task->blocked_next;
        }
    }
    restore_irq(flags);
}

void task_wake_task(task_t *task) {
    uint32_t flags = save_irq_disable();
    task_t **link = &blocked_tasks;

    if (!task) {
        restore_irq(flags);
        return;
    }

    while (*link) {
        if (*link == task) {
            *link = task->blocked_next;
            add_ready_task(task);
            break;
        }
        link = &(*link)->blocked_next;
    }
    restore_irq(flags);
}

/* Mark every task of `process` for termination and wake the blocked ones.
 *
 * The ready ones are handled by process_check_kill() on the next timer tick,
 * but marking them too closes the window where such a task parks in a wait
 * before any tick lands on it. The blocked ones are the whole point: a task
 * sitting in `while (cond) block;` never becomes current, so without this it
 * stays asleep for good and its process never finishes dying.
 *
 * A woken task resumes inside task_block_current(), and the kill_pending check
 * in task_block_killable() takes it out from there. */
uint32_t task_kill_blocked(struct process *process) {
    uint32_t flags = save_irq_disable();
    uint32_t woken = 0;
    task_t **link = &blocked_tasks;
    task_t *ready = (task_t *)ready_queue;

    if (!process) {
        restore_irq(flags);
        return 0;
    }

    if (ready) {
        task_t *task = ready;

        do {
            if (task->process == process) task->kill_pending = 1;
            task = task->next;
        } while (task && task != ready);
    }

    while (*link) {
        task_t *task = *link;

        if (task->process == process) {
            task->kill_pending = 1;
            *link = task->blocked_next;   /* now points at the next entry */
            add_ready_task(task);
            woken++;
            continue;
        }
        link = &task->blocked_next;
    }
    restore_irq(flags);
    return woken;
}

int task_kill_pending(void) {
    task_t *task = (task_t *)current_task;

    return task && task->kill_pending;
}

uint32_t task_get_blocked_count(void) {
    uint32_t flags = save_irq_disable();
    uint32_t count = 0;
    task_t *task = blocked_tasks;

    while (task) {
        count++;
        task = task->blocked_next;
    }
    restore_irq(flags);
    return count;
}

void schedule(void) {
    if (!current_task) return;

    reap_retired_task();
    
    task_t *prev_task = (task_t*)current_task;
    task_t *next_task = prev_task->next;
    
    if (prev_task == next_task) return; /* Only one task */

    activate_task(next_task);
    switch_task(&prev_task->esp, next_task->esp);
}

void task_exit(int32_t status) {
    task_t *task = (task_t*)current_task;
    task_t *previous;
    task_t *next;

    (void)status;
#ifndef HOSTED_TEST
    __asm__ volatile("cli");
#endif

    if (!task) {
        terminal_writestring("Cannot exit: tasking is not initialized.\n");
        while (1) { __asm__ volatile("hlt"); }
    }

    previous = task;
    while (previous->next != task) {
        previous = previous->next;
    }

    if (previous == task) {
        terminal_writestring("Last task exited. System halted.\n");
        current_task = 0;
        ready_queue = 0;
        while (1) { __asm__ volatile("hlt"); }
    }

    reap_retired_task();
    next = task->next;
    previous->next = next;
    if (ready_queue == task) {
        ready_queue = next;
    }
    current_task = next;
    retired_task = task;

    activate_task(next);
    if (task->on_exit) task->on_exit(task, status);

    switch_task(&task->esp, next->esp);

    while (1) { __asm__ volatile("hlt"); }
}
