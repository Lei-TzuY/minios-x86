#include <setjmp.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>

/*
 * Signal delivery and return (syscall.c: signal_deliver, sys_sigreturn).
 *
 * Two of this project's four P0s live on this path. F2 was signal_deliver
 * writing a frame through an unvalidated user esp; F14 was sys_sigreturn
 * reading one back the same way. Both were about the ADDRESS being attacker
 * controlled. CAP10 tests the pointer validators themselves, but nothing has
 * ever tested the frame build and restore as a pair -- and nothing has looked
 * at the CONTENTS of what sigreturn copies back.
 *
 * The contents matter as much as the address, because one field of the saved
 * context is not data at all: eflags is loaded by the iret that returns to
 * ring 3, and iret executed at CPL 0 loads the IOPL field from the stack image
 * rather than preserving it. Everything sigreturn writes into regs->eflags
 * therefore lands in the real EFLAGS register of a ring-3 program.
 *
 * The suite drives the two functions directly against a real, mapped user
 * stack, so the frame is built and read at genuine addresses rather than
 * simulated.
 */

/* The host's <signal.h> defines NSIG too, and this kernel's is a different
 * number. Drop the host's so process.h's definition -- the one every signal
 * loop below is bounded by -- applies without a redefinition warning. */
#undef NSIG

#include "../elf_loader.h"
#include "../process.h"

/* --- the current process --------------------------------------------------- */

static process_t  g_proc;
static process_t *g_current = &g_proc;

process_t *process_get_current(void) { return g_current; }

int paging_user_range_mapped(uint32_t vaddr, uint32_t size) {
    (void)vaddr; (void)size;
    return 1;
}

int process_ext_reserved(const process_t *proc, uint32_t vaddr) {
    uint32_t index;

    if (!proc || vaddr < USER_EXT_BASE || vaddr >= USER_EXT_TOP) return 0;
    index = (vaddr - USER_EXT_BASE) >> 12;
    return (proc->ext_map[index >> 5] >> (index & 31)) & 1U;
}

/* --- the scheduler, only as far as these paths reach it -------------------- */

#include "../task.h"

static int     g_task_exit_calls;
static int32_t g_task_exit_status;
static int     g_block_calls;
static int     g_kill_pending;
static int     g_wake_calls;

/*
 * How a call left, when it did not simply return:
 *   1  task_exit -- the kernel refused and killed the process
 *   2  it touched memory outside the mapped user stack
 *
 * The second case is the one worth naming. In the kernel that access happens
 * with CS still 0x08, so the page-fault handler treats it as a kernel fault
 * and halts the machine rather than killing one process -- F2 and F14 were
 * both exactly that. Letting the test process die instead would leave the
 * mutation script reporting a signal number where an assertion should be.
 */
static sigjmp_buf g_fault_jmp;
static int        g_jmp_reason;
static int        g_faults_seen;

static void on_sigsegv(int sig) {
    (void)sig;
    g_faults_seen++;
    g_jmp_reason = 2;
    siglongjmp(g_fault_jmp, 2);
}

void task_exit(int32_t status) {
    g_task_exit_calls++;
    g_task_exit_status = status;
    g_jmp_reason = 1;
    siglongjmp(g_fault_jmp, 1);
}

int task_kill_pending(void) { return g_kill_pending; }

/* SIGSTOP parks here until SIGCONT is posted. The stub delivers the SIGCONT on
 * the first park so the loop can make progress, and counts the parks so a test
 * can tell "waited" from "did not wait". */
static int g_deliver_sigcont_on_block;

/* A stopped process that is never released is a process that never runs
 * again. Past this many parks the harness says so by name rather than letting
 * the loop spin until something outside the test notices. */
#define STOP_PARK_LIMIT 64

void task_block_current(const void *channel) {
    (void)channel;
    g_block_calls++;
    if (g_deliver_sigcont_on_block) {
        g_deliver_sigcont_on_block = 0;
        g_proc.sig_pending |= (1u << SIGCONT);
    }
    if (g_block_calls > STOP_PARK_LIMIT) {
        g_jmp_reason = 3;
        siglongjmp(g_fault_jmp, 3);
    }
}

void task_wake_all(const void *channel) { (void)channel; g_wake_calls++; }
void task_wake_one(const void *channel) { (void)channel; g_wake_calls++; }
void task_wake_task(task_t *task) { (void)task; g_wake_calls++; }
task_t *task_get_current(void) { return NULL; }

static uint32_t g_timer_ticks;
uint32_t timer_get_ticks(void) { return g_timer_ticks; }

#include "../syscall.c"

#include "test.h"

/* --- a real user stack ----------------------------------------------------- */

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static int map_user_stack(void) {
    void *want = (void *)(uintptr_t)USER_STACK_BOTTOM;
    size_t len = USER_STACK_TOP - USER_STACK_BOTTOM;
    void *got = mmap(want, len, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);

    return got == want;
}

static int map_mmap_stack(void) {
    void *want = (void *)(uintptr_t)USER_EXT_BASE;
    size_t len = 4 * 0x1000U;
    void *got = mmap(want, len, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);

    return got == want;
}

static int g_mmap_stack_available;

#define EFL_CF   0x00000001u
#define EFL_BIT1 0x00000002u
#define EFL_ZF   0x00000040u
#define EFL_TF   0x00000100u
#define EFL_IF   0x00000200u
#define EFL_DF   0x00000400u
#define EFL_OF   0x00000800u
#define EFL_IOPL 0x00003000u
#define EFL_NT   0x00004000u
#define EFL_VM   0x00020000u
#define EFL_AC   0x00040000u

static registers_t g_regs;

static void reset_world(void) {
    memset(&g_proc, 0, sizeof(g_proc));
    g_proc.pid = 1;
    g_proc.slot = 0;
    g_proc.state = PROCESS_RUNNING;
    g_proc.sig_trampoline = 0x00301234;
    g_current = &g_proc;

    memset(&g_regs, 0, sizeof(g_regs));
    g_regs.cs = 0x1B;                        /* returning to ring 3 */
    g_regs.eflags = EFL_IF | EFL_BIT1;
    g_regs.useresp = USER_STACK_TOP - 0x100;
    g_regs.eip = 0x00300100;

    g_task_exit_calls = 0;
    g_task_exit_status = 0;
    g_faults_seen = 0;
    g_block_calls = 0;
    g_kill_pending = 0;
    g_wake_calls = 0;
    g_deliver_sigcont_on_block = 0;
    g_timer_ticks = 0;
}

static void reserve_mmap_stack(void) {
    g_proc.ext_map[0] = 0xFU;   /* the four pages mapped by map_mmap_stack() */
}

/* Returns 0 (returned normally), 1 (task_exit) or 2 (faulted); see above. */
#define RUN_MAY_EXIT(stmt)                                                   \
    (g_jmp_reason = 0,                                                       \
     sigsetjmp(g_fault_jmp, 1) == 0 ? ((stmt), g_jmp_reason) : g_jmp_reason)

/* --- F25: the contents of the restored context ----------------------------- */

static void test_sigreturn_sanitises_eflags(void) {
    sigcontext_t *sc;
    uint32_t sp;

    /*
     * F25. sys_sigreturn copies the saved eflags straight into regs->eflags,
     * and the iret that follows loads it into the real register. Executed at
     * CPL 0 -- which is where the kernel is when it returns to a user process
     * -- iret takes the IOPL field from the stack image instead of preserving
     * it, so every bit the user put there becomes real:
     *
     *   IOPL=3  ring 3 may execute in/out on any port, and cli/sti. A program
     *           can drive the disk controller directly, or simply cli and stop
     *           the machine -- no timer, no scheduler, nothing.
     *   VM      returns into virtual-8086 mode.
     *   NT      makes the next iret attempt a task switch through the TSS.
     *   IF=0    returns to ring 3 with interrupts disabled: the same freeze.
     *
     * SYS_SIGRETURN is reachable by any program issuing int $0x80 directly; it
     * does not have to be inside a handler, and the frame it names lives in
     * the program's own stack, so the F14 bounds check passes. The address was
     * validated; the contents never were.
     */
    TEST("sigreturn does not let a program choose its own EFLAGS");
    reset_world();

    sp = USER_STACK_TOP - 0x200;
    sc = (sigcontext_t *)(sp + 4);
    memset(sc, 0, sizeof(*sc));
    sc->eip = 0x00300400;
    sc->useresp = USER_STACK_TOP - 0x80;
    /* Everything a program would want and must not get. */
    sc->eflags = EFL_IOPL | EFL_NT | EFL_VM | EFL_AC | EFL_TF | EFL_CF | EFL_ZF;

    g_regs.useresp = sp;
    g_proc.in_signal = 1;

    CHECK_EQ(RUN_MAY_EXIT(sys_sigreturn(&g_regs)), 0);

    /* The privileged bits must not survive. */
    CHECK_EQ(g_regs.eflags & EFL_IOPL, 0);
    CHECK_EQ(g_regs.eflags & EFL_NT, 0);
    CHECK_EQ(g_regs.eflags & EFL_VM, 0);

    /* Interrupts must be on when the program resumes, whatever it asked for. */
    CHECK(g_regs.eflags & EFL_IF);

    /* The arithmetic flags are the program's own business and must survive:
     * code after a signal handler legitimately depends on them. */
    CHECK(g_regs.eflags & EFL_CF);
    CHECK(g_regs.eflags & EFL_ZF);

    /* The rest of the context is restored as before. */
    CHECK_EQ(g_regs.eip, 0x00300400);
    CHECK_EQ(g_regs.useresp, USER_STACK_TOP - 0x80);
    CHECK_EQ(g_proc.in_signal, 0);
}

static void test_sigreturn_cannot_clear_if(void) {
    sigcontext_t *sc;
    uint32_t sp;

    TEST("sigreturn cannot return to ring 3 with interrupts off");
    reset_world();

    sp = USER_STACK_TOP - 0x200;
    sc = (sigcontext_t *)(sp + 4);
    memset(sc, 0, sizeof(*sc));
    sc->eflags = 0;                          /* IF clear */
    sc->eip = 0x00300400;
    sc->useresp = USER_STACK_TOP - 0x80;
    g_regs.useresp = sp;

    CHECK_EQ(RUN_MAY_EXIT(sys_sigreturn(&g_regs)), 0);

    /* A ring-3 program running with IF clear is a stopped machine: no timer
     * tick, so the scheduler never runs again. This is the same outcome as
     * F22, reached by a different route. */
    CHECK(g_regs.eflags & EFL_IF);
}

/* --- sigreturn bounds (F14) ------------------------------------------------ */

static void test_sigreturn_bounds(void) {
    TEST("sigreturn: the frame must lie in the user stack");
    reset_world();

    /* Below the stack. Reason 1 means the kernel refused; reason 2 would
     * mean it went ahead and touched the address. */
    g_regs.useresp = USER_STACK_BOTTOM - 4;
    CHECK_EQ(RUN_MAY_EXIT(sys_sigreturn(&g_regs)), 1);
    CHECK_EQ(g_task_exit_calls, 1);
    CHECK_EQ(g_faults_seen, 0);

    /* Above the stack. */
    reset_world();
    g_regs.useresp = USER_STACK_TOP + 4;
    CHECK_EQ(RUN_MAY_EXIT(sys_sigreturn(&g_regs)), 1);

    /* In range, but the frame would run off the top -- the arithmetic that
     * catches this must not wrap. */
    reset_world();
    g_regs.useresp = USER_STACK_TOP - 4;
    CHECK_EQ(RUN_MAY_EXIT(sys_sigreturn(&g_regs)), 1);
    CHECK_EQ(g_faults_seen, 0);

    /* Exactly enough room is accepted. */
    reset_world();
    {
        uint32_t sp = USER_STACK_TOP - (4 + sizeof(sigcontext_t));
        sigcontext_t *sc = (sigcontext_t *)(sp + 4);

        memset(sc, 0, sizeof(*sc));
        sc->eip = 0x00300400;
        sc->eflags = EFL_IF;
        sc->useresp = USER_STACK_TOP - 0x40;
        g_regs.useresp = sp;
        CHECK_EQ(RUN_MAY_EXIT(sys_sigreturn(&g_regs)), 0);
        CHECK_EQ(g_regs.eip, 0x00300400);
    }

    /* With no current process it is a no-op rather than a fault. */
    reset_world();
    g_current = NULL;
    CHECK_EQ(RUN_MAY_EXIT(sys_sigreturn(&g_regs)), 0);
    CHECK_EQ(g_task_exit_calls, 0);
    g_current = &g_proc;
}

static void test_sigreturn_from_mmap_thread_stack(void) {
    sigcontext_t *sc;
    uint32_t sp;
    int outcome;

    if (!g_mmap_stack_available) {
        TEST("sigreturn mmap-stack test skipped (mapping unavailable)");
        CHECK(1);
        return;
    }

    TEST("sigreturn restores a context from a reserved mmap thread stack");
    reset_world();
    reserve_mmap_stack();
    sp = USER_EXT_BASE + 4 * 0x1000U - 0x200U;
    sc = (sigcontext_t *)(sp + 4);
    memset(sc, 0, sizeof(*sc));
    sc->eip = 0x00300400;
    sc->eflags = EFL_IF;
    sc->useresp = USER_EXT_BASE + 4 * 0x1000U - 0x80U;
    g_regs.useresp = sp;
    g_proc.in_signal = 1;

    outcome = RUN_MAY_EXIT(sys_sigreturn(&g_regs));
    CHECK_EQ(outcome, 0);
    if (outcome != 0) return;
    CHECK_EQ(g_regs.eip, 0x00300400);
    CHECK_EQ(g_regs.useresp, USER_EXT_BASE + 4 * 0x1000U - 0x80U);
    CHECK_EQ(g_proc.in_signal, 0);
}

/* --- delivery -------------------------------------------------------------- */

static void test_deliver_builds_a_frame(void) {
    uint32_t original_esp;
    uint32_t original_eip;
    sigcontext_t *sc;

    TEST("delivery builds the frame the trampoline expects");
    reset_world();
    original_esp = g_regs.useresp;
    original_eip = g_regs.eip;
    g_regs.eax = 0x11111111;
    g_regs.ebx = 0x22222222;
    g_regs.ebp = 0x33333333;
    g_proc.sig_handler[SIGUSR1] = 0x00300500;
    g_proc.sig_pending = (1u << SIGUSR1);

    CHECK_EQ(RUN_MAY_EXIT(signal_deliver(&g_regs)), 0);

    /* Control transfers to the handler ... */
    CHECK_EQ(g_regs.eip, 0x00300500);
    CHECK_EQ(g_proc.in_signal, 1);
    CHECK_EQ(g_proc.sig_pending, 0);

    /* ... on a stack holding [context][signum][trampoline]. */
    CHECK_EQ(g_regs.useresp, original_esp - sizeof(sigcontext_t) - 8);
    CHECK_EQ(*(uint32_t *)g_regs.useresp, g_proc.sig_trampoline);
    CHECK_EQ(*(uint32_t *)(g_regs.useresp + 4), (uint32_t)SIGUSR1);

    sc = (sigcontext_t *)(g_regs.useresp + 8);
    CHECK_EQ(sc->eip, original_eip);
    CHECK_EQ(sc->eax, 0x11111111);
    CHECK_EQ(sc->ebx, 0x22222222);
    CHECK_EQ(sc->ebp, 0x33333333);
    CHECK_EQ(sc->useresp, original_esp);
}

static void test_deliver_on_mmap_thread_stack(void) {
    uint32_t original_esp;
    int outcome;

    if (!g_mmap_stack_available) {
        TEST("signal mmap-stack delivery test skipped (mapping unavailable)");
        CHECK(1);
        return;
    }

    TEST("delivery builds a frame on a reserved mmap thread stack");
    reset_world();
    reserve_mmap_stack();
    original_esp = USER_EXT_BASE + 4 * 0x1000U - 0x100U;
    g_regs.useresp = original_esp;
    g_proc.sig_handler[SIGUSR1] = 0x00300500;
    g_proc.sig_pending = (1u << SIGUSR1);

    outcome = RUN_MAY_EXIT(signal_deliver(&g_regs));
    CHECK_EQ(outcome, 0);
    if (outcome != 0) return;
    CHECK_EQ(g_regs.eip, 0x00300500);
    CHECK_EQ(g_regs.useresp, original_esp - sizeof(sigcontext_t) - 8);
    CHECK_EQ(*(uint32_t *)g_regs.useresp, g_proc.sig_trampoline);
}

static void test_deliver_round_trip(void) {
    uint32_t esp0, eip0, eax0;

    TEST("deliver then return restores the interrupted context exactly");
    reset_world();
    g_regs.eax = 0xAAAA0001;
    g_regs.ebx = 0xBBBB0002;
    g_regs.ecx = 0xCCCC0003;
    g_regs.edx = 0xDDDD0004;
    g_regs.esi = 0xEEEE0005;
    g_regs.edi = 0xFFFF0006;
    g_regs.ebp = 0x12340007;
    g_regs.eflags = EFL_IF | EFL_BIT1 | EFL_ZF;
    esp0 = g_regs.useresp;
    eip0 = g_regs.eip;
    eax0 = g_regs.eax;

    g_proc.sig_handler[SIGUSR1] = 0x00300500;
    g_proc.sig_pending = (1u << SIGUSR1);
    CHECK_EQ(RUN_MAY_EXIT(signal_deliver(&g_regs)), 0);

    /* The handler returns: its `ret` pops the trampoline address that
     * delivery pushed, leaving esp on the signum. The trampoline then issues
     * SYS_SIGRETURN from there, which is why sigreturn skips four bytes to
     * find the context. Model that step, or the restore reads the frame one
     * slot out and every register comes back shifted. */
    g_regs.useresp += 4;
    CHECK_EQ(RUN_MAY_EXIT(sys_sigreturn(&g_regs)), 0);

    CHECK_EQ(g_regs.eip, eip0);
    CHECK_EQ(g_regs.eax, eax0);
    CHECK_EQ(g_regs.ebx, 0xBBBB0002);
    CHECK_EQ(g_regs.ecx, 0xCCCC0003);
    CHECK_EQ(g_regs.edx, 0xDDDD0004);
    CHECK_EQ(g_regs.esi, 0xEEEE0005);
    CHECK_EQ(g_regs.edi, 0xFFFF0006);
    CHECK_EQ(g_regs.ebp, 0x12340007);
    CHECK_EQ(g_regs.useresp, esp0);
    CHECK(g_regs.eflags & EFL_ZF);         /* the program's own flags survive */
    CHECK(g_regs.eflags & EFL_IF);
    CHECK_EQ(g_proc.in_signal, 0);
}

static void test_deliver_only_to_user_mode(void) {
    TEST("delivery only happens on the way back to ring 3");
    reset_world();
    g_proc.sig_handler[SIGUSR1] = 0x00300500;
    g_proc.sig_pending = (1u << SIGUSR1);

    /* Interrupted kernel code: the frame would be built on whatever esp the
     * kernel happened to have. */
    g_regs.cs = 0x08;
    CHECK_EQ(RUN_MAY_EXIT(signal_deliver(&g_regs)), 0);
    CHECK_EQ(g_proc.in_signal, 0);
    CHECK(g_proc.sig_pending & (1u << SIGUSR1));   /* still pending */

    /* Already inside a handler: no nesting. */
    g_regs.cs = 0x1B;
    g_proc.in_signal = 1;
    CHECK_EQ(RUN_MAY_EXIT(signal_deliver(&g_regs)), 0);
    CHECK(g_proc.sig_pending & (1u << SIGUSR1));

    /* Nothing pending is a no-op. */
    g_proc.in_signal = 0;
    g_proc.sig_pending = 0;
    CHECK_EQ(RUN_MAY_EXIT(signal_deliver(&g_regs)), 0);
    CHECK_EQ(g_proc.in_signal, 0);
}

static void test_deliver_one_signal_per_return(void) {
    TEST("one signal per return to user mode");
    reset_world();
    g_proc.sig_handler[SIGUSR1] = 0x00300500;
    g_proc.sig_handler[SIGALRM] = 0x00300600;
    g_proc.sig_pending = (1u << SIGUSR1) | (1u << SIGALRM);

    CHECK_EQ(RUN_MAY_EXIT(signal_deliver(&g_regs)), 0);

    /* The lower-numbered signal goes first, and the other stays pending: a
     * second frame built on top of the first would run the second handler
     * with the first one's return address. */
    CHECK_EQ(g_regs.eip, 0x00300500);
    CHECK_EQ(g_proc.sig_pending, (1u << SIGALRM));
    CHECK_EQ(g_proc.in_signal, 1);
}

static void test_default_actions(void) {
    TEST("default actions");

    /* No handler: everything but SIGCHLD terminates the process. */
    reset_world();
    g_proc.sig_pending = (1u << SIGUSR1);
    CHECK_EQ(RUN_MAY_EXIT(signal_deliver(&g_regs)), 1);
    CHECK_EQ(g_task_exit_calls, 1);
    CHECK_EQ(g_task_exit_status, -128 - SIGUSR1);

    /* SIGCHLD is ignored by default -- a parent that never installed a
     * handler must not be killed by its own child exiting. */
    reset_world();
    g_proc.sig_pending = (1u << SIGCHLD);
    CHECK_EQ(RUN_MAY_EXIT(signal_deliver(&g_regs)), 0);
    CHECK_EQ(g_task_exit_calls, 0);
    CHECK_EQ(g_proc.sig_pending, 0);
    CHECK_EQ(g_proc.in_signal, 0);

    /* Explicitly ignored (handler == 1). */
    reset_world();
    g_proc.sig_handler[SIGUSR1] = 1;
    g_proc.sig_pending = (1u << SIGUSR1);
    CHECK_EQ(RUN_MAY_EXIT(signal_deliver(&g_regs)), 0);
    CHECK_EQ(g_task_exit_calls, 0);
    CHECK_EQ(g_proc.in_signal, 0);
}

static void test_deliver_bounds(void) {
    TEST("delivery: the frame must fit in the user stack");

    /* An esp the program set to something outside its stack. Writing the
     * frame there would fault with CS still 0x08, which the page-fault
     * handler treats as a kernel fault and answers by halting the machine --
     * F2 exactly. */
    reset_world();
    g_proc.sig_handler[SIGUSR1] = 0x00300500;
    g_proc.sig_pending = (1u << SIGUSR1);
    g_regs.useresp = 0x1000;                   /* far below the stack */
    CHECK_EQ(RUN_MAY_EXIT(signal_deliver(&g_regs)), 1);
    CHECK_EQ(g_task_exit_status, -128 - SIGUSR1);
    CHECK_EQ(g_faults_seen, 0);

    reset_world();
    g_proc.sig_handler[SIGUSR1] = 0x00300500;
    g_proc.sig_pending = (1u << SIGUSR1);
    g_regs.useresp = USER_STACK_TOP + 0x1000;
    CHECK_EQ(RUN_MAY_EXIT(signal_deliver(&g_regs)), 1);

    /* In range but with no room below for the frame: the check has to be a
     * subtraction that cannot wrap. */
    reset_world();
    g_proc.sig_handler[SIGUSR1] = 0x00300500;
    g_proc.sig_pending = (1u << SIGUSR1);
    g_regs.useresp = USER_STACK_BOTTOM + 4;
    CHECK_EQ(RUN_MAY_EXIT(signal_deliver(&g_regs)), 1);
    CHECK_EQ(g_faults_seen, 0);

    /* Exactly enough room is accepted. */
    reset_world();
    g_proc.sig_handler[SIGUSR1] = 0x00300500;
    g_proc.sig_pending = (1u << SIGUSR1);
    g_regs.useresp = USER_STACK_BOTTOM + sizeof(sigcontext_t) + 8;
    CHECK_EQ(RUN_MAY_EXIT(signal_deliver(&g_regs)), 0);
    CHECK_EQ(g_regs.eip, 0x00300500);
    CHECK_EQ(g_regs.useresp, USER_STACK_BOTTOM);

    /* An address in the mmap window is not a stack until the process has
     * actually reserved every page that would hold the frame. */
    reset_world();
    g_proc.sig_handler[SIGUSR1] = 0x00300500;
    g_proc.sig_pending = (1u << SIGUSR1);
    g_regs.useresp = USER_EXT_BASE + 0x1000U;
    CHECK_EQ(RUN_MAY_EXIT(signal_deliver(&g_regs)), 1);
    CHECK_EQ(g_faults_seen, 0);
}

static void test_job_control(void) {
    TEST("SIGSTOP parks until SIGCONT");
    reset_world();
    g_proc.sig_pending = (1u << SIGSTOP);
    g_deliver_sigcont_on_block = 1;

    /* Reason 3 would mean it parked and was never let go: a stopped process
     * that no SIGCONT can release never runs again. */
    CHECK_EQ(RUN_MAY_EXIT(signal_deliver(&g_regs)), 0);

    /* It really parked, and the SIGCONT that arrived released it and was
     * consumed rather than being left pending for the next return. */
    CHECK(g_block_calls > 0);
    CHECK(g_block_calls <= STOP_PARK_LIMIT);
    CHECK_EQ(g_proc.stopped, 0);
    CHECK_EQ(g_proc.sig_pending & (1u << SIGCONT), 0);
    CHECK_EQ(g_proc.in_signal, 0);         /* SIGSTOP runs no handler */

    /* SIGCONT on its own, with no handler, is simply consumed. */
    reset_world();
    g_proc.sig_pending = (1u << SIGCONT);
    CHECK_EQ(RUN_MAY_EXIT(signal_deliver(&g_regs)), 0);
    CHECK_EQ(g_block_calls, 0);
    CHECK_EQ(g_proc.in_signal, 0);
    CHECK_EQ(g_task_exit_calls, 0);
}

static void test_alarm_wrap_boundaries(void) {
    TEST("alarm deadline may wrap exactly to tick zero");
    reset_world();
    g_timer_ticks = UINT32_MAX;
    CHECK_EQ(sys_alarm(1), 0);
    /* Cancelling immediately must report the full tick still remaining. A zero
     * deadline is valid here, not an inactive-alarm sentinel. */
    CHECK_EQ(sys_alarm(0), 1);

    TEST("alarm rejects delays outside signed modular range");
    reset_world();
    g_timer_ticks = 100;
    CHECK_EQ(sys_alarm(10), 0);
    CHECK_EQ(sys_alarm(0x80000000U), -1);
    /* A rejected replacement must leave the existing alarm intact. */
    CHECK_EQ(sys_alarm(0), 10);
}

int main(void) {
    if (!map_user_stack()) {
        printf("SKIP signal: could not map the user stack region\n");
        return 0;
    }
    g_mmap_stack_available = map_mmap_stack();
    signal(SIGSEGV, on_sigsegv);
    signal(SIGBUS, on_sigsegv);

    test_sigreturn_sanitises_eflags();
    test_sigreturn_cannot_clear_if();
    test_sigreturn_bounds();
    test_sigreturn_from_mmap_thread_stack();

    test_deliver_builds_a_frame();
    test_deliver_on_mmap_thread_stack();
    test_deliver_round_trip();
    test_deliver_only_to_user_mode();
    test_deliver_one_signal_per_return();
    test_default_actions();
    test_deliver_bounds();
    test_job_control();
    test_alarm_wrap_boundaries();

    TEST_REPORT("signal");
}
