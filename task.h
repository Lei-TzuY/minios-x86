#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include "paging.h"

/* CPU Context stored on the stack during a task switch */
typedef struct {
    uint32_t edi;
    uint32_t esi;
    uint32_t ebx;
    uint32_t ebp;
    uint32_t eip;
} context_t;

struct task;
struct process;
typedef void (*task_exit_callback_t)(struct task *, int32_t);

typedef enum task_state {
    TASK_READY = 0,
    TASK_BLOCKED,
} task_state_t;

/* Task Control Block */
typedef struct task {
    uint32_t esp;        /* Stack pointer */
    struct task* next;   /* Next task in linked list */
    struct task* blocked_next;
    uint32_t id;         /* Task ID */
    void *stack_bottom;  /* Allocated stack block, or NULL for the main task */
    uint32_t kernel_stack_top; /* Ring 0 stack loaded through the TSS */
    address_space_t *address_space;
    struct process *process;
    task_exit_callback_t on_exit; /* Called when task exits, or NULL */
    const void *wait_channel;
    task_state_t state;
    uint32_t user_entry; /* thread entry point (0 for non-thread tasks) */
    uint32_t user_stack; /* thread user stack top (0 for non-thread tasks) */
} task_t;

void tasking_init(void);
task_t* create_task(void (*entry)(void), task_exit_callback_t on_exit,
                    address_space_t *address_space, struct process *process,
                    uint32_t user_entry, uint32_t user_stack);
task_t *task_get_current(void);
void task_block_current(const void *wait_channel);
void task_wake_one(const void *wait_channel);
void task_wake_all(const void *wait_channel);
/* Unblock one SPECIFIC task (no-op if it is not currently blocked).
 * Use this when the kernel must nudge a particular task -- e.g. delivering a
 * signal to a chosen process -- instead of task_wake_one(), which wakes an
 * arbitrary waiter on a channel. Several unrelated tasks routinely share a
 * wait channel (the shell blocks on a child's process_t in process_wait while
 * that same child blocks on its own process_t in process_waitpid), so picking
 * "some waiter" can wake the wrong one and leave the intended task asleep. */
void task_wake_task(task_t *task);
uint32_t task_get_blocked_count(void);
void schedule(void);
void task_exit(int32_t status) __attribute__((noreturn));

#endif
