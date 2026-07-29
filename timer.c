#include "timer.h"
#include "isr.h"
#include "io.h"
#include "irq.h"
#include "process.h"
#include "vga.h"
#include "utils.h"
#include "task.h"

#define MAX_SLEEPING_TASKS 16

typedef struct {
    task_t *task;
    uint32_t wake_tick;
} sleep_entry_t;

uint32_t timer_ticks = 0;
static sleep_entry_t sleeping_tasks[MAX_SLEEPING_TASKS];

static int tick_reached(uint32_t current, uint32_t deadline) {
    return (int32_t)(current - deadline) >= 0;
}

static void timer_callback(registers_t *regs) {
    (void)regs;
    timer_ticks++;

    for (uint32_t i = 0; i < MAX_SLEEPING_TASKS; i++) {
        if (sleeping_tasks[i].task &&
            tick_reached(timer_ticks, sleeping_tasks[i].wake_tick)) {
            task_wake_all(&sleeping_tasks[i]);
            sleeping_tasks[i].task = NULL;
        }
    }
    
    process_account_tick(); /* Charge this tick to the running process */
    process_check_kill();   /* Kill foreground process on Ctrl+C request */
    process_check_alarms(timer_ticks);   /* Raise SIGALRM on expired alarms */
    /* Preemptive multitasking: call the scheduler on every timer tick */
    schedule();
}

void timer_install(void) {
    /* IRQ0 is the timer */
    register_interrupt_handler(32, timer_callback);

    /* Get the PIT value: hardware clock at 1193180 Hz */
    uint32_t divisor = 1193180 / 100; /* 100 Hz */

    /* Send the command byte */
    outb(0x43, 0x36);

    /* Divisor has to be sent byte-wise, so split here into upper/lower bytes. */
    uint8_t l = (uint8_t)(divisor & 0xFF);
    uint8_t h = (uint8_t)( (divisor>>8) & 0xFF );

    /* Send the frequency divisor */
    outb(0x40, l);
    outb(0x40, h);
}

void timer_wait(uint32_t ticks) {
    (void)timer_sleep(ticks);
}

int timer_sleep(uint32_t ticks) {
    uint32_t flags;
    task_t *task;

    if (ticks == 0) return 0;
    if (ticks > 0x7FFFFFFFU) return -1;

    flags = save_irq_disable();
    task = task_get_current();
    if (!task) {
        restore_irq(flags);
        return -1;
    }
    /* Already flagged for termination: leave now rather than take a sleep slot
     * that nothing would ever wake (same reasoning as task_block_killable). */
    if (task_kill_pending()) task_exit(TASK_KILL_STATUS);

    for (uint32_t i = 0; i < MAX_SLEEPING_TASKS; i++) {
        if (!sleeping_tasks[i].task) {
            sleeping_tasks[i].task = task;
            sleeping_tasks[i].wake_tick = timer_ticks + ticks;
            task_block_current(&sleeping_tasks[i]);
            /* This is the one wait that cannot use task_block_killable(): the
             * sleep slot must be handed back before the task goes away, or it
             * would stay reserved with a pointer to freed memory until the
             * original deadline passed. Only clear it while it is still ours --
             * if the deadline fired, timer_callback already cleared it and
             * another sleeper may have taken the slot over. */
            if (task_kill_pending()) {
                if (sleeping_tasks[i].task == task) sleeping_tasks[i].task = NULL;
                task_exit(TASK_KILL_STATUS);
            }
            restore_irq(flags);
            return 0;
        }
    }

    restore_irq(flags);
    return -1;
}

uint32_t timer_get_ticks(void) {
    return timer_ticks;
}

uint32_t timer_get_sleeping_count(void) {
    uint32_t flags = save_irq_disable();
    uint32_t count = 0;

    for (uint32_t i = 0; i < MAX_SLEEPING_TASKS; i++) {
        if (sleeping_tasks[i].task) count++;
    }

    restore_irq(flags);
    return count;
}
