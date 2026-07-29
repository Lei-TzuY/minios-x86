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
    /* Set by task_kill_blocked(): this task must terminate as soon as it is
     * running again, rather than resume whatever it was waiting for. */
    volatile uint8_t kill_pending;
} task_t;

/* Exit status given to a task terminated by a kill request (128 + SIGINT),
 * matching the shell convention used by process_check_kill(). */
#define TASK_KILL_STATUS (-130)

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
/* Mark every task belonging to `process` for termination and wake the blocked
 * ones, so each leaves from inside its wait loop. Returns how many were woken.
 * This is the only way to reach a task parked in `while (cond) block;`: it
 * never becomes the current task, so the per-tick check that terminates a
 * killed process (process_check_kill) can never see it. */
uint32_t task_kill_blocked(struct process *process);
/* Non-zero when the current task has been marked for termination. */
int task_kill_pending(void);
uint32_t task_get_blocked_count(void);
void schedule(void);
void task_exit(int32_t status) __attribute__((noreturn));

/* Block on `wait_channel` and, once woken, honour a kill that landed while the
 * task was parked instead of looping back into the wait. Every blocking wait
 * in the kernel should use this rather than task_block_current() directly; the
 * one exception is a caller that must release a resource before dying (see
 * timer_sleep), which does the same check itself after cleaning up. */
static inline void task_block_killable(const void *wait_channel) {
    /* Check before blocking as well as after. A task can be flagged while it is
     * running -- task_kill_blocked() marks the killed process's runnable tasks
     * too -- and if it then entered a wait it would park with nothing left to
     * wake it: the kill has already been delivered, and the per-tick check only
     * looks at the current task. Flagged means "never wait again". */
    if (task_kill_pending()) task_exit(TASK_KILL_STATUS);
    task_block_current(wait_channel);
    if (task_kill_pending()) task_exit(TASK_KILL_STATUS);
}

#endif
