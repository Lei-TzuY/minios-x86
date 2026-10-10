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

/* Keep the reference-count trace while using the actual VFS dispatch and RAMFS
 * unlink protection for redirect lifetime regressions. */
#define open_fs backend_open_fs
#define close_fs backend_close_fs
#include "../fs.c"
#undef open_fs
#undef close_fs
#include "../ramfs.c"

static address_space_t image_space;
static task_t child_task;
static process_t *published_child;
static process_t first_run;
static fs_node_t out_node, in_node;
static pipe_t out_pipe, in_pipe;
static int first_runs, loads, load_if, destroys, destroy_if;
static int image_fails, task_fails, node_refs;
static int unlink_during_load, pipe_fails, live_pipes, live_allocations;
static int fail_load_at, fail_task_at, creates, exit_during_wait;

static int redirect_is_pinned(const char *path) {
    fs_node_t *node = resolve_fs(path);
    CHECK(node != NULL);
    if (!node) return 0;
    CHECK((node->impl & ~RAMFS_DYNAMIC) != 0);
    if ((node->impl & ~RAMFS_DYNAMIC) == 0) return 0;
    CHECK_EQ(unlink_fs(path), -1);
    return 1;
}

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
    if (unlink_during_load &&
        (!redirect_is_pinned("/held-out") ||
         !redirect_is_pinned("/held-in"))) return NULL;
    if (image_fails || (fail_load_at && loads == fail_load_at)) return NULL;
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
    creates++;
    if (task_fails || (fail_task_at && creates == fail_task_at)) {
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
int task_kill_pending(void) { return 0; }
void task_block_current(const void *channel) {
    if (exit_during_wait) {
        process_t *process = (process_t *)channel;
        CHECK(process_find(process->pid) == process);
        CHECK_EQ(process->state, PROCESS_RUNNING);
        process_finish_exit(process, 0);
        return;
    }
    CHECK(0 && "unexpected wait in shell launch fixture");
    abort();
}
void task_exit(int32_t status) { (void)status; abort(); }
void enter_user_mode(uint32_t entry, uint32_t esp) { (void)entry; (void)esp; }
void syscall_close_user_files(process_t *process) { (void)process; }
void process_test_observe_release(process_t *process) { (void)process; }
void terminal_writestring(const char *message) { (void)message; }
void terminal_putchar(char c) { (void)c; }
void terminal_write_dec(uint32_t value) { (void)value; }
void terminal_setcolor(uint8_t color) { (void)color; }
void keyboard_set_foreground(int32_t pid) { (void)pid; }
void keyboard_clear_foreground(void) {}

void *kmalloc(size_t size) {
    void *memory = malloc(size);
    if (memory) live_allocations++;
    return memory;
}
void kfree(void *memory) {
    if (memory) live_allocations--;
    free(memory);
}

void paging_destroy_user_address_space(address_space_t *space) {
    CHECK(space == &image_space);
    destroys++;
    destroy_if = irq_enabled;
}

void open_fs(fs_node_t *node) {
    if (node->open) CHECK_EQ(irq_enabled, 0);
    node_refs++;
    backend_open_fs(node);
}
void close_fs(fs_node_t *node) {
    CHECK(node_refs > 0);
    node_refs--;
    backend_close_fs(node);
}
pipe_t *pipe_create(void) {
    if (pipe_fails) return NULL;
    pipe_t *pipe = calloc(1, sizeof(*pipe));
    CHECK(pipe != NULL);
    if (pipe) {
        pipe->readers = pipe->writers = 1;
        live_pipes++;
    }
    return pipe;
}
static void release_test_pipe(pipe_t *pipe) {
    if (pipe != &out_pipe && pipe != &in_pipe &&
        !pipe->readers && !pipe->writers) {
        live_pipes--;
        free(pipe);
    }
}
void pipe_close_read(pipe_t *pipe) {
    CHECK(pipe->readers > 0);
    pipe->readers--;
    release_test_pipe(pipe);
}
void pipe_close_write(pipe_t *pipe) {
    CHECK(pipe->writers > 0);
    pipe->writers--;
    release_test_pipe(pipe);
}

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
    unlink_during_load = pipe_fails = 0;
    fail_load_at = fail_task_at = creates = exit_during_wait = 0;
    load_if = destroy_if = -1;
    irq_enabled = initial_if;
    strcpy(shell_cwd, "/disk/work");
}

static void redirect_fixture(void) {
    reset(1);
    strcpy(shell_cwd, "/");
    CHECK(fs_root == NULL);
    CHECK_EQ(live_allocations, 0);
    ramfs_init();
    CHECK(create_fs("/held-in") != NULL);
}

static void release_children(void) {
    for (int32_t pid = 1; pid < next_pid; pid++) {
        if (process_find(pid)) {
            process_finish_exit(process_find(pid), 0);
        }
    }
}

static void finish_redirect_fixture(int has_output) {
    CHECK_EQ(node_refs, 0);
    CHECK_EQ(live_pipes, 0);
    if (has_output) CHECK_EQ(unlink_fs("/held-out"), 0);
    CHECK_EQ(unlink_fs("/held-in"), 0);
    CHECK_EQ(ramfs_get_node_count(), 1);
    kfree(fs_root);
    fs_root = NULL;
    node_count = 0;
    memset(ramfs_nodes, 0, sizeof(ramfs_nodes));
    CHECK_EQ(live_allocations, 0);
    CHECK_EQ(irq_enabled, 1);
}

static void redirect_lifetime(int failure, int append) {
    const char *argv[] = {"cat"};
    redirect_fixture();
    TEST("real redirect nodes survive image loading and transfer ownership");
    if (append) {
        fs_node_t *output = create_fs("/held-out");
        CHECK(output != NULL);
        CHECK_EQ(write_fs(output, 0, 3, (uint8_t *)"old"), 3);
    }
    image_fails = failure == 1;
    task_fails = failure == 2;
    unlink_during_load = 1;
    int32_t pid = shell_spawn_command(1, argv, "held-out", append, "held-in");
    CHECK_EQ(load_if, 1);
    if (failure) {
        CHECK_EQ(pid, -1);
        CHECK_EQ(node_refs, 0);
        CHECK_EQ(first_runs, 0);
    } else {
        CHECK_EQ(pid, 1);
        CHECK_EQ(node_refs, 2); /* Only the child retains each reference. */
        CHECK(first_run.stdout_node == resolve_fs("/held-out"));
        CHECK(first_run.stdin_node == resolve_fs("/held-in"));
        CHECK_EQ(first_run.stdout_offset, append ? 3 : 0);
        release_children();
    }
    finish_redirect_fixture(1);
}

static void redirect_failures(void) {
    const char *argv[] = {"cat"};
    redirect_fixture();
    TEST("input failure releases the prepared output reference");
    CHECK_EQ(shell_spawn_command(1, argv, "held-out", 0, "missing"), -1);
    CHECK_EQ(loads, 0);
    finish_redirect_fixture(1);

    redirect_fixture();
    TEST("output failure never loads or retains the input");
    CHECK_EQ(shell_spawn_command(1, argv, "missing/held-out", 0, "held-in"), -1);
    CHECK_EQ(loads, 0);
    finish_redirect_fixture(0);
}

static void shared_redirect(void) {
    const char *argv[] = {"cat"};
    redirect_fixture();
    TEST("one node used by both streams retains two child references");
    CHECK_EQ(shell_spawn_command(1, argv, "held-in", 1, "held-in"), 1);
    CHECK_EQ(node_refs, 2);
    CHECK(first_run.stdout_node == first_run.stdin_node);
    CHECK(first_run.stdin_node != NULL);
    if (first_run.stdin_node)
        CHECK_EQ(first_run.stdin_node->impl & ~RAMFS_DYNAMIC, 2 * RAMFS_OPEN_REF);
    release_children();
    finish_redirect_fixture(0);
}

static void redirect_disabled_if(void) {
    uint32_t offset;
    redirect_fixture();
    irq_enabled = 0;
    TEST("redirect success and errors preserve disabled incoming IF");
    fs_node_t *output = shell_open_out("held-out", 0, &offset);
    CHECK(output != NULL);
    CHECK_EQ(irq_enabled, 0);
    fs_node_t *input = shell_open_in("held-in");
    CHECK(input != NULL);
    CHECK_EQ(irq_enabled, 0);
    CHECK(shell_open_out("missing/out", 0, &offset) == NULL);
    CHECK_EQ(irq_enabled, 0);
    CHECK(shell_open_in("missing") == NULL);
    CHECK_EQ(irq_enabled, 0);
    shell_close_redirects(output, input);
    restore_irq(1u << 9);
    finish_redirect_fixture(1);
}

static void pipeline_redirect_lifetime(int failure) {
    const char *argv[] = {"cat", "<", "held-in", "|", "cat", ">", "held-out"};
    redirect_fixture();
    TEST("pipeline holds last-stage redirect across every image load");
    pipe_fails = failure == 1;
    image_fails = failure == 2;
    fail_load_at = failure == 3 ? 2 : 0;
    fail_task_at = failure == 4 ? 2 : 0;
    exit_during_wait = failure >= 3;
    unlink_during_load = 1;
    shell_run_pipeline(argv, 7, 1);
    if (failure) {
        CHECK_EQ(node_refs, 0);
        CHECK_EQ(process_get_running_count(), 0);
    } else {
        CHECK_EQ(loads, 2);
        CHECK_EQ(node_refs, 2);
        CHECK_EQ(live_pipes, 1);
        process_t *first = process_find(1), *last = process_find(2);
        CHECK(first != NULL);
        CHECK(last != NULL);
        if (first) CHECK(first->stdin_node == resolve_fs("/held-in"));
        if (last) CHECK(last->stdout_node == resolve_fs("/held-out"));
        release_children();
    }
    finish_redirect_fixture(1);
}

static void pipeline_redirect_failures(void) {
    const char *syntax[] = {"cat", "<", "held-in", "|", "cat", ">"};
    redirect_fixture();
    TEST("later pipeline syntax error releases the prepared input");
    shell_run_pipeline(syntax, 6, 1);
    CHECK_EQ(loads, 0);
    finish_redirect_fixture(0);

    const char *missing[] = {"cat", "<", "held-in", "|", "cat", ">", "missing/out"};
    redirect_fixture();
    TEST("last-stage open error releases the prepared input");
    shell_run_pipeline(missing, 7, 1);
    CHECK_EQ(loads, 0);
    finish_redirect_fixture(0);
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

    redirect_lifetime(0, 0);
    redirect_lifetime(0, 1);
    redirect_lifetime(1, 0);
    redirect_lifetime(2, 0);
    redirect_failures();
    shared_redirect();
    redirect_disabled_if();
    pipeline_redirect_lifetime(0);
    pipeline_redirect_lifetime(1);
    pipeline_redirect_lifetime(2);
    pipeline_redirect_lifetime(3);
    pipeline_redirect_lifetime(4);
    pipeline_redirect_failures();
    TEST_REPORT("shell-spawn");
}
