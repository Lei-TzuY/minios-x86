#include <stdint.h>

/*
 * mmap/sbrk ownership and lifetime.
 *
 * The mmap bitmap is shared by every task in one process, while the actual
 * frames live in its address space.  That looks like it could leave a small
 * preemption window between releasing the bitmap and unmapping the PTE.  It
 * does not in this kernel: int 0x80 is an interrupt gate, so IF is already
 * clear for the entire syscall.  The model below makes that otherwise hidden
 * cross-module contract executable: a sibling becomes runnable only when the
 * simulated syscall gate returns, and must then see the PTE already gone.
 * If the ABI ever switches to a trap gate or permits preemption in syscalls,
 * this test identifies the exact point that needs an explicit outer lock.
 */

struct process;
int32_t process_ext_alloc(struct process *proc, uint32_t npages);

static int g_irq_depth;
static int g_irq_disable_calls;
static int g_irq_restore_calls;
static int g_run_contender;
static int g_in_contender;
static int g_page_present;
static int g_contender_saw_present;
static int32_t g_contender_addr;
static struct process *g_contender_process;

/* Replace irq.h: hosted tests need observable nesting and the point at which
 * another task becomes schedulable. */
#define IRQ_H
static inline uint32_t save_irq_disable(void) {
    g_irq_disable_calls++;
    g_irq_depth++;
    return 0x200;
}
static inline void restore_irq(uint32_t flags) {
    (void)flags;
    g_irq_restore_calls++;
    g_irq_depth--;
    if (g_irq_depth == 0 && g_run_contender && !g_in_contender) {
        g_run_contender = 0;
        g_in_contender = 1;
        g_contender_saw_present = g_page_present;
        g_contender_addr = process_ext_alloc(g_contender_process, 1);
        g_in_contender = 0;
    }
}

static void enter_syscall_gate(void) {
    g_irq_disable_calls++;
    g_irq_depth++;
}

static void leave_syscall_gate(void) { restore_irq(0x200); }

#include "../process.c"

/* Only the reachable syscall paths are retained by --gc-sections. */
static task_t g_task;
static process_t *g_current;
task_t *task_get_current(void) { return &g_task; }
void paging_unmap_user_page(address_space_t *space, uint32_t vaddr) {
    (void)space;
    (void)vaddr;
    g_page_present = 0;
}

#include "../syscall.c"

#include "test.h"

static void reset(process_t *process) {
    memset(process, 0, sizeof(*process));
    memset(&g_task, 0, sizeof(g_task));
    g_task.process = process;
    g_current = process;
    process->heap_start = 0x8000;
    process->heap_break = 0x8000;
    g_irq_depth = 0;
    g_irq_disable_calls = 0;
    g_irq_restore_calls = 0;
    g_run_contender = 0;
    g_in_contender = 0;
    g_page_present = 0;
    g_contender_saw_present = -1;
    g_contender_addr = 0;
    g_contender_process = process;
}

static void test_sbrk_contract(void) {
    process_t process;

    reset(&process);
    TEST("sbrk: query, grow, shrink, and bounds preserve ownership");
    CHECK_EQ(sys_sbrk(0), 0x8000);
    CHECK_EQ(sys_sbrk(0x1800), 0x8000);
    CHECK_EQ(process.heap_break, 0x9800);
    CHECK_EQ(sys_sbrk(-0x800), 0x9800);
    CHECK_EQ(process.heap_break, 0x9000);
    CHECK_EQ(sys_sbrk(-0x1001), -1);       /* cannot cross heap_start */
    CHECK_EQ(process.heap_break, 0x9000);

    process.heap_break = USER_HEAP_MAX - 4;
    CHECK_EQ(sys_sbrk(4), (int32_t)(USER_HEAP_MAX - 4));
    CHECK_EQ(process.heap_break, USER_HEAP_MAX);
    CHECK_EQ(sys_sbrk(1), -1);             /* cannot enter the stack guard */
    CHECK_EQ(process.heap_break, USER_HEAP_MAX);
    CHECK_EQ(sys_sbrk((int32_t)0x80000000U), -1); /* INT32_MIN is defined */
    CHECK_EQ(process.heap_break, USER_HEAP_MAX);
}

static void test_mmap_bitmap_contract(void) {
    process_t process;
    int32_t a, b, c;

    reset(&process);
    TEST("mmap bitmap: first-fit reuse and invalid frees are atomic");
    a = sys_mmap(2);
    b = sys_mmap(3);
    CHECK_EQ(a, (int32_t)USER_EXT_BASE);
    CHECK_EQ(b, (int32_t)(USER_EXT_BASE + 2 * 0x1000U));
    CHECK_EQ(process_ext_reserved(&process, (uint32_t)a), 1);
    CHECK_EQ(process_ext_reserved(&process, (uint32_t)b + 2 * 0x1000U), 1);
    /* Allocation provenance is intentionally page-granular: crossing `b` is
     * allowed, but a range containing an actually free page must fail without
     * clearing the valid prefix. */
    CHECK_EQ(process_ext_free(&process, (uint32_t)a, 6), -1);
    CHECK_EQ(process_ext_reserved(&process, (uint32_t)a), 1);
    CHECK_EQ(process_ext_free(&process, (uint32_t)a, 2), 0);
    c = sys_mmap(2);
    CHECK_EQ(c, a);
    CHECK_EQ(process_ext_free(&process, (uint32_t)a, 2), 0);
    CHECK_EQ(process_ext_free(&process, (uint32_t)a, 2), -1); /* double free */
    CHECK_EQ(process_ext_free(&process, USER_EXT_TOP, 1), -1);
    CHECK_EQ(process_ext_free(&process, USER_EXT_BASE + 1, 1), -1);
    CHECK_EQ(g_irq_depth, 0);
}

static void test_munmap_is_atomic_under_the_syscall_gate(void) {
    process_t process;
    int32_t addr;

    reset(&process);
    addr = sys_mmap(1);
    g_page_present = 1;                 /* the page was demand-mapped */
    enter_syscall_gate();                /* int 0x80 has cleared IF */
    g_run_contender = 1;                /* scheduler may run only after iret */

    TEST("munmap: syscall gate covers bitmap release through PTE teardown");
    CHECK_EQ(sys_munmap((uint32_t)addr, 1), 0);
    CHECK_EQ(g_contender_addr, 0);       /* no task can run inside the syscall */
    CHECK_EQ(g_page_present, 0);
    CHECK_EQ(process_ext_reserved(&process, (uint32_t)addr), 0);
    leave_syscall_gate();
    CHECK_EQ(g_contender_addr, addr);   /* the sibling reused the hole */
    CHECK_EQ(g_contender_saw_present, 0); /* never inherits the old mapping */
    CHECK_EQ(g_page_present, 0);
    CHECK_EQ(process_ext_reserved(&process, (uint32_t)addr), 1);
    CHECK_EQ(g_irq_depth, 0);
    CHECK(g_irq_disable_calls == g_irq_restore_calls);
}

int main(void) {
    test_sbrk_contract();
    test_mmap_bitmap_contract();
    test_munmap_is_atomic_under_the_syscall_gate();
    TEST_REPORT("vm-lifecycle");
}
