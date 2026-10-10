#include <stdint.h>

/* Model a timer scheduling the child at the first IF-enabled publication
 * boundary. The real launch and stream-ownership code is linked below. */
#define IRQ_H
static int irq_enabled;
static void observe_preemption(void);

static uint32_t save_irq_disable(void) {
    uint32_t flags = irq_enabled ? (1u << 9) : 0;
    irq_enabled = 0;
    return flags;
}

static void restore_irq(uint32_t flags) {
    if (flags & (1u << 9)) {
        irq_enabled = 1;
        observe_preemption();
    }
}

#include "../kernel.c"
#include "../process.c"
#include "test.h"
#include <stdlib.h>

static address_space_t image_space;
static task_t child_task;
static process_t *published_child;
static process_t first_run;
static fs_node_t out_node, in_node;
static pipe_t out_pipe, in_pipe;
static int first_runs, loads, load_if, destroys, destroy_if;
static int image_fails, task_fails, node_refs;

static void observe_preemption(void) {
    if (published_child && !first_runs) {
        first_run = *published_child;
        first_runs++;
    }
}

address_space_t *elf_load_image(const char *name, int argc, const char **argv,
                                uint32_t *entry, uint32_t *user_esp,
                                uint32_t *heap_base) {
    (void)name; (void)argc; (void)argv;
    loads++;
    load_if = irq_enabled;
    if (image_fails) return NULL;
    *entry = USER_LOAD_BASE;
    *user_esp = USER_STACK_TOP;
    *heap_base = USER_LOAD_BASE + 4096;
    return &image_space;
}

task_t *create_task(void (*entry)(void), task_exit_callback_t on_exit,
                    address_space_t *space, struct process *process,
                    uint32_t user_entry, uint32_t user_stack) {
    uint32_t flags = save_irq_disable();
    (void)entry; (void)user_entry; (void)user_stack;
    if (task_fails) {
        restore_irq(flags);
        return NULL;
    }
    child_task.process = process;
    child_task.address_space = space;
    child_task.on_exit = on_exit;
    child_task.state = TASK_READY;
    published_child = process;
    restore_irq(flags);
    return &child_task;
}

task_t *task_get_current(void) { return NULL; }
void task_wake_all(const void *channel) { (void)channel; }
void task_wake_task(task_t *task) { (void)task; }
void task_exit(int32_t status) { (void)status; abort(); }
void enter_user_mode(uint32_t entry, uint32_t esp) { (void)entry; (void)esp; }
void syscall_close_user_files(process_t *process) { (void)process; }
void process_test_observe_release(process_t *process) { (void)process; }
void terminal_writestring(const char *message) { (void)message; }

void paging_destroy_user_address_space(address_space_t *space) {
    CHECK(space == &image_space);
    destroys++;
    destroy_if = irq_enabled;
}

void open_fs(fs_node_t *node) { (void)node; node_refs++; }
void close_fs(fs_node_t *node) { (void)node; node_refs--; }
void pipe_close_read(pipe_t *pipe) { pipe->readers--; }
void pipe_close_write(pipe_t *pipe) { pipe->writers--; }

static void reset(int initial_if) {
    memset(processes, 0, sizeof(processes));
    memset(&child_task, 0, sizeof(child_task));
    memset(&first_run, 0, sizeof(first_run));
    memset(&out_pipe, 0, sizeof(out_pipe));
    memset(&in_pipe, 0, sizeof(in_pipe));
    out_pipe.writers = in_pipe.readers = 1;
    next_pid = 1;
    peak_process_count = 0;
    published_child = NULL;
    first_runs = loads = destroys = node_refs = 0;
    image_fails = task_fails = 0;
    load_if = destroy_if = -1;
    irq_enabled = initial_if;
    strcpy(shell_cwd, "/disk/work");
}

static void successful_setup(int initial_if, int pipeline) {
    const char *argv[] = {"cat"};
    reset(initial_if);
    TEST(pipeline ? "pipeline before first run" : "redirect before first run");
    CHECK_EQ(shell_spawn_argv(1, argv, &out_node, 73, &in_node,
                              pipeline ? &out_pipe : NULL,
                              pipeline ? &in_pipe : NULL), 1);
    CHECK_EQ(load_if, initial_if);
    CHECK_EQ(irq_enabled, initial_if);
    CHECK_EQ(first_runs, initial_if);
    if (!initial_if) restore_irq(1u << 9);
    CHECK_EQ(first_runs, 1);
    CHECK_STREQ(first_run.cwd, "/disk/work");
    CHECK(first_run.stdout_node == &out_node);
    CHECK(first_run.stdin_node == &in_node);
    CHECK_EQ(first_run.stdout_offset, 73);
    CHECK_EQ(first_run.stdin_offset, 0);
    CHECK(first_run.stdout_pipe == (pipeline ? &out_pipe : NULL));
    CHECK(first_run.stdin_pipe == (pipeline ? &in_pipe : NULL));
    CHECK(first_run.task == &child_task);
    CHECK(first_run.address_space == &image_space);
    CHECK_EQ(node_refs, 2);
    CHECK_EQ(destroys, 0);
    CHECK_EQ(out_pipe.writers, 1);
    CHECK_EQ(in_pipe.readers, 1);
}

static void failed_setup(int initial_if, int fail_image) {
    const char *argv[] = {"cat"};
    reset(initial_if);
    TEST(fail_image ? "image failure retains caller ownership"
                    : "task failure retains caller ownership");
    image_fails = fail_image;
    task_fails = !fail_image;
    CHECK_EQ(shell_spawn_argv(1, argv, &out_node, 73, &in_node,
                              &out_pipe, &in_pipe), -1);
    CHECK_EQ(irq_enabled, initial_if);
    CHECK_EQ(load_if, initial_if);
    CHECK_EQ(first_runs, 0);
    CHECK_EQ(process_get_running_count(), 0);
    CHECK_EQ(node_refs, 0);
    CHECK_EQ(out_pipe.writers, 1);
    CHECK_EQ(in_pipe.readers, 1);
    CHECK_EQ(destroys, !fail_image);
    if (!fail_image) CHECK_EQ(destroy_if, initial_if);
}

int main(void) {
    for (int initial_if = 0; initial_if <= 1; initial_if++) {
        successful_setup(initial_if, 0);
        successful_setup(initial_if, 1);
        failed_setup(initial_if, 0);
        failed_setup(initial_if, 1);
    }
    reset(1);
    TEST("invalid arguments do not allocate or publish");
    const char *empty[] = {NULL};
    CHECK_EQ(shell_spawn_argv(0, empty, NULL, 0, NULL, NULL, NULL), -1);
    CHECK_EQ(shell_spawn_argv(1, NULL, NULL, 0, NULL, NULL, NULL), -1);
    CHECK_EQ(shell_spawn_argv(1, empty, NULL, 0, NULL, NULL, NULL), -1);
    CHECK_EQ(loads, 0);
    CHECK_EQ(first_runs, 0);
    CHECK_EQ(irq_enabled, 1);
    TEST_REPORT("shell-spawn");
}
