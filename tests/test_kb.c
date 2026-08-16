#include "test.h"

#include <setjmp.h>

/*
 * The keyboard driver (kb.c): a 128-entry ring buffer written by an interrupt
 * handler and drained by a blocking read, plus a modifier state machine and the
 * Ctrl+C path that terminates the foreground process.
 *
 * Every one of those is pure logic, and none of it had a test. What the QEMU
 * suite does exercise is one narrow lane: `send_keys` types short lines that
 * are consumed immediately, so the buffer never approaches full, the write
 * index never wraps past 128, and no key is ever dropped. The interesting
 * states are exactly the ones a shell session never reaches.
 *
 * Two things make this worth pinning down:
 *
 *   - The ring buffer is the one place in the kernel where an interrupt
 *     handler and a task touch the same structure. Its "full" test sacrifices
 *     one slot (next == read means full, not empty); an off-by-one there
 *     either drops a key that fits or, worse, lets the writer lap the reader
 *     and hand back characters in the wrong order.
 *
 *   - Ctrl+C chooses between a catchable SIGINT and a forced kill, and that
 *     decision is made inside an interrupt handler against a pid the driver
 *     stores. Sending the wrong one to the wrong process is not something the
 *     shell test can distinguish -- both make the program stop.
 *
 * kb.c is included directly (as tests/test_rtc.c does) to reach its statics.
 * io.h is suppressed by pre-defining its include guard so port reads can be
 * driven by the test instead of returning a constant 0: that is how scancodes
 * get in. Nothing in the kernel headers is modified.
 */

/* --- port I/O: replace io.h entirely -------------------------------------- */
#define IO_H
#include <stdint.h>

static uint8_t g_port_value;          /* what the next inb(0x60) returns */
static int     g_port_reads;

static inline uint8_t inb(uint16_t port) {
    (void)port;
    g_port_reads++;
    return g_port_value;
}
static inline void outb(uint16_t port, uint8_t val) { (void)port; (void)val; }
static inline uint16_t inw(uint16_t port) { (void)port; return 0; }
static inline void outw(uint16_t port, uint16_t val) { (void)port; (void)val; }
static inline void io_wait(void) { }

/* --- stubs for everything kb.c calls -------------------------------------- */

#include "../isr.h"
#include "../task.h"

static isr_t     g_installed_handler;
static uint8_t   g_installed_vector;

void register_interrupt_handler(uint8_t n, isr_t handler) {
    g_installed_vector = n;
    g_installed_handler = handler;
}

/* Echo. The driver prints printable keys as they arrive; Ctrl+C prints "^C"
 * through a different call, which is how the test tells the two apart. */
static char g_echo[512];
static int  g_echo_len;
static int  g_writestring_calls;

static void echo_putc(char c) {
    if (g_echo_len < (int)sizeof(g_echo) - 1) g_echo[g_echo_len++] = c;
    g_echo[g_echo_len] = '\0';
}

void terminal_putchar(char c) { echo_putc(c); }

void terminal_writestring(const char *s) {
    g_writestring_calls++;
    while (*s) echo_putc(*s++);
}

/* Scheduler. task_block_killable() is a static inline in task.h built from
 * these three, so stubbing them covers it. */
static int      g_wake_calls;
static int      g_block_calls;
static int      g_kill_pending;
static int      g_blocks_before_input;   /* deliver a key after N blocks */
static jmp_buf  g_exit_jmp;
static int      g_exited;
static int32_t  g_exit_status;

static void feed_scancode(uint8_t scancode);

void task_wake_one(const void *channel) { (void)channel; g_wake_calls++; }
void task_wake_all(const void *channel) { (void)channel; }
void task_wake_task(task_t *task)       { (void)task; }

int task_kill_pending(void) { return g_kill_pending; }

/* Standing in for "the machine ran while this task was parked": after the
 * configured number of blocks, a key arrives through the real interrupt path,
 * so the wakeup the driver waits for is produced the same way the hardware
 * would produce it. */
void task_block_current(const void *channel) {
    (void)channel;
    g_block_calls++;
    if (g_block_calls >= g_blocks_before_input && g_blocks_before_input > 0) {
        g_blocks_before_input = 0;
        feed_scancode(0x2C);            /* 'z' */
    }
}

void task_exit(int32_t status) {
    g_exited = 1;
    g_exit_status = status;
    longjmp(g_exit_jmp, 1);
}

/* Signal delivery decisions made from inside the interrupt handler. */
static int     g_has_handler;
static int32_t g_signalled_pid  = -1;
static int     g_signalled_sig;
static int32_t g_killed_pid     = -1;
static int     g_send_calls;
static int     g_kill_calls;

int process_has_sighandler(int32_t pid, int signum) {
    (void)pid; (void)signum;
    return g_has_handler;
}

int process_send_signal(int32_t pid, int signum) {
    g_send_calls++;
    g_signalled_pid = pid;
    g_signalled_sig = signum;
    return 0;
}

void process_request_kill(int32_t pid) {
    g_kill_calls++;
    g_killed_pid = pid;
}

#include "../kb.c"

/* --- helpers -------------------------------------------------------------- */

/* Drive one scancode through the real interrupt handler. */
static void feed_scancode(uint8_t scancode) {
    registers_t regs;

    g_port_value = scancode;
    g_installed_handler(&regs);
}

static void reset_all(void) {
    /* The driver's ring indices are file statics; drain rather than poke them,
     * so every test starts from a genuinely empty buffer reached the same way
     * the kernel would reach it. */
    char sink;
    while (keyboard_try_read(&sink)) { }

    ctrl_held = 0;
    shift_held = 0;
    keyboard_clear_foreground();

    g_echo_len = 0;
    g_echo[0] = '\0';
    g_writestring_calls = 0;
    g_wake_calls = 0;
    g_block_calls = 0;
    g_kill_pending = 0;
    g_blocks_before_input = 0;
    g_port_reads = 0;
    g_has_handler = 0;
    g_signalled_pid = -1;
    g_signalled_sig = 0;
    g_killed_pid = -1;
    g_send_calls = 0;
    g_kill_calls = 0;
    g_exited = 0;
    g_exit_status = 0;
}

/* Scancodes used below (US set 1). */
#define SC_A          0x1E
#define SC_C          0x2E
#define SC_Z          0x2C
#define SC_1          0x02
#define SC_ENTER      0x1C
#define SC_LSHIFT     0x2A
#define SC_RSHIFT     0x36
#define SC_CTRL       0x1D
#define SC_F1         0x3B
#define SC_CAPSLOCK   0x3A
#define SC_RELEASE    0x80

/* --- tests ---------------------------------------------------------------- */

static void test_install(void) {
    TEST("install");
    keyboard_install();
    CHECK(g_installed_handler != NULL);
    CHECK_EQ(g_installed_vector, 33);      /* IRQ1 -> vector 33 */
}

static void test_basic_mapping(void) {
    char c = 0;

    TEST("scancode mapping");
    reset_all();

    feed_scancode(SC_A);
    CHECK_EQ(g_port_reads, 1);             /* the handler reads the port once */
    CHECK_EQ(keyboard_try_read(&c), 1);
    CHECK_EQ(c, 'a');
    CHECK_EQ(keyboard_try_read(&c), 0);    /* and only one character arrived */

    feed_scancode(SC_1);
    CHECK_EQ(keyboard_try_read(&c), 1);
    CHECK_EQ(c, '1');

    feed_scancode(SC_ENTER);
    CHECK_EQ(keyboard_try_read(&c), 1);
    CHECK_EQ(c, '\n');

    /* Printable keys are echoed as they arrive. */
    CHECK_STREQ(g_echo, "a1\n");
}

static void test_release_and_dead_keys(void) {
    char c = 0;

    TEST("releases and unmapped keys");
    reset_all();

    /* Any scancode with bit 7 set is a key release and must produce nothing --
     * otherwise every keystroke would be delivered twice. */
    feed_scancode(SC_A);
    feed_scancode(SC_A | SC_RELEASE);
    CHECK_EQ(keyboard_try_read(&c), 1);
    CHECK_EQ(c, 'a');
    CHECK_EQ(keyboard_try_read(&c), 0);

    /* Keys that map to 0 in the table (F1, caps lock) are dropped rather than
     * delivered as a NUL byte. */
    feed_scancode(SC_F1);
    feed_scancode(SC_CAPSLOCK);
    CHECK_EQ(keyboard_try_read(&c), 0);
    CHECK_STREQ(g_echo, "a");

    /* The highest scancode a press can carry is 0x7F, and the tables hold
     * exactly 128 entries -- so the index is in range by construction. Walk
     * every one of them to prove no press faults or emits a NUL. */
    for (int sc = 0; sc < 0x80; sc++) {
        char out = 0;
        int  got;

        if (sc == SC_CTRL || sc == SC_LSHIFT || sc == SC_RSHIFT) continue;
        feed_scancode((uint8_t)sc);
        got = keyboard_try_read(&out);
        CHECK(got == (kbdus[sc] != 0));
        if (got) CHECK_EQ((unsigned char)out, kbdus[sc]);
    }
}

static void test_shift_state_machine(void) {
    char c = 0;

    TEST("shift state machine");
    reset_all();

    feed_scancode(SC_LSHIFT);
    feed_scancode(SC_A);
    CHECK_EQ(keyboard_try_read(&c), 1);
    CHECK_EQ(c, 'A');

    /* Shift is held until its own release arrives, not consumed by one key. */
    feed_scancode(SC_Z);
    CHECK_EQ(keyboard_try_read(&c), 1);
    CHECK_EQ(c, 'Z');

    feed_scancode(SC_LSHIFT | SC_RELEASE);
    feed_scancode(SC_A);
    CHECK_EQ(keyboard_try_read(&c), 1);
    CHECK_EQ(c, 'a');

    /* The right shift is a different scancode and must work the same way --
     * including releasing the state the left one set, which is what the shared
     * `shift_held` flag means. */
    feed_scancode(SC_RSHIFT);
    feed_scancode(SC_1);
    CHECK_EQ(keyboard_try_read(&c), 1);
    CHECK_EQ(c, '!');
    feed_scancode(SC_RSHIFT | SC_RELEASE);
    feed_scancode(SC_1);
    CHECK_EQ(keyboard_try_read(&c), 1);
    CHECK_EQ(c, '1');

    /* A shift press is itself not a character. */
    reset_all();
    feed_scancode(SC_LSHIFT);
    CHECK_EQ(keyboard_try_read(&c), 0);
    CHECK_EQ(g_echo_len, 0);
}

static void test_ctrl_c_signals_handler(void) {
    char c = 0;

    TEST("ctrl+c with a handler installed");
    reset_all();
    keyboard_set_foreground(42);
    g_has_handler = 1;

    feed_scancode(SC_CTRL);
    feed_scancode(SC_C);

    /* A process that installed a SIGINT handler gets the catchable signal, not
     * a forced kill -- that difference is the entire point of the branch, and
     * from the shell both merely look like "the program stopped". */
    CHECK_EQ(g_send_calls, 1);
    CHECK_EQ(g_signalled_pid, 42);
    CHECK_EQ(g_signalled_sig, SIGINT);
    CHECK_EQ(g_kill_calls, 0);

    /* The reader is woken and sees the interrupt character, so a task parked
     * in keyboard_read() does not stay parked. */
    CHECK(g_wake_calls > 0);
    CHECK_EQ(keyboard_try_read(&c), 1);
    CHECK_EQ(c, '\3');

    /* "^C" is echoed, and the \3 itself is not printed. */
    CHECK_STREQ(g_echo, "^C\n");
}

static void test_ctrl_c_force_kills(void) {
    TEST("ctrl+c without a handler");
    reset_all();
    keyboard_set_foreground(7);
    g_has_handler = 0;

    feed_scancode(SC_CTRL);
    feed_scancode(SC_C);

    CHECK_EQ(g_kill_calls, 1);
    CHECK_EQ(g_killed_pid, 7);
    CHECK_EQ(g_send_calls, 0);
}

static void test_ctrl_c_without_foreground(void) {
    char c = 0;

    TEST("ctrl+c with no foreground process");
    reset_all();
    keyboard_clear_foreground();

    feed_scancode(SC_CTRL);
    feed_scancode(SC_C);

    /* Nothing to signal -- and in particular no call with pid -1, which would
     * be handed to process_find() as a search key. */
    CHECK_EQ(g_send_calls, 0);
    CHECK_EQ(g_kill_calls, 0);

    /* The interrupt character is still queued: the kernel shell reads it. */
    CHECK_EQ(keyboard_try_read(&c), 1);
    CHECK_EQ(c, '\3');
}

static void test_ctrl_released_restores_typing(void) {
    char c = 0;

    TEST("ctrl release");
    reset_all();
    keyboard_set_foreground(5);

    feed_scancode(SC_CTRL);
    feed_scancode(SC_CTRL | SC_RELEASE);
    feed_scancode(SC_C);

    /* With ctrl released, 'c' is an ordinary character again. */
    CHECK_EQ(g_kill_calls, 0);
    CHECK_EQ(g_send_calls, 0);
    CHECK_EQ(keyboard_try_read(&c), 1);
    CHECK_EQ(c, 'c');

    /* Ctrl+<other key> is not an interrupt either: only 'c' is. */
    reset_all();
    keyboard_set_foreground(5);
    feed_scancode(SC_CTRL);
    feed_scancode(SC_A);
    CHECK_EQ(g_kill_calls, 0);
    CHECK_EQ(keyboard_try_read(&c), 1);
    CHECK_EQ(c, 'a');
}

static void test_extended_scancodes(void) {
    char c = 0;

    TEST("extended (0xE0-prefixed) scancodes");
    reset_all();

    /*
     * The driver has no notion of the 0xE0 prefix that real hardware sends
     * ahead of the arrow keys, the right-hand modifiers, and the keypad. 0xE0
     * has bit 7 set, so it falls into the key-release branch, where its low
     * seven bits (0x60) match no modifier and it is discarded. The byte that
     * follows is then handled as if it had arrived unprefixed.
     *
     * Every consequence of that turns out to be benign, and this test is here
     * to say so deliberately rather than leave it as an accident nobody
     * checked -- and to describe what must not regress if anyone ever does add
     * prefix handling.
     */

    /* Right Ctrl (0xE0 0x1D) lands on the same index as left Ctrl, so it works
     * -- including its release. Ctrl+C from the right-hand key must therefore
     * interrupt the foreground process just like the left one. */
    keyboard_set_foreground(11);
    feed_scancode(0xE0);
    feed_scancode(SC_CTRL);
    feed_scancode(SC_C);
    CHECK_EQ(g_kill_calls, 1);
    CHECK_EQ(g_killed_pid, 11);

    reset_all();
    feed_scancode(0xE0);
    feed_scancode(SC_CTRL);
    feed_scancode(0xE0);
    feed_scancode(SC_CTRL | SC_RELEASE);
    feed_scancode(SC_C);
    CHECK_EQ(g_kill_calls, 0);             /* released, so 'c' is a character */
    CHECK_EQ(keyboard_try_read(&c), 1);
    CHECK_EQ(c, 'c');

    /* Arrow keys, Home/End/Delete and friends all land on table entries that
     * are 0, so they are silently dropped -- no stray characters in the line
     * the shell is reading. */
    reset_all();
    {
        static const uint8_t extended_navigation[] = {
            0x48, 0x50, 0x4B, 0x4D,        /* up, down, left, right */
            0x47, 0x4F, 0x49, 0x51,        /* home, end, page up/down */
            0x52, 0x53,                    /* insert, delete */
        };

        for (unsigned i = 0; i < sizeof(extended_navigation); i++) {
            feed_scancode(0xE0);
            feed_scancode(extended_navigation[i]);
        }
        CHECK_EQ(keyboard_try_read(&c), 0);
        CHECK_EQ(g_echo_len, 0);
    }

    /* The keypad keys that DO carry a character land on the right one, because
     * the extended code reuses the index of the equivalent main-block key. */
    reset_all();
    feed_scancode(0xE0);
    feed_scancode(SC_ENTER);               /* keypad Enter */
    CHECK_EQ(keyboard_try_read(&c), 1);
    CHECK_EQ(c, '\n');

    feed_scancode(0xE0);
    feed_scancode(0x35);                   /* keypad / */
    CHECK_EQ(keyboard_try_read(&c), 1);
    CHECK_EQ(c, '/');

    /* A bare 0xE0 on its own contributes nothing and leaves no state behind:
     * the next ordinary key is unaffected. */
    reset_all();
    feed_scancode(0xE0);
    CHECK_EQ(keyboard_try_read(&c), 0);
    feed_scancode(SC_A);
    CHECK_EQ(keyboard_try_read(&c), 1);
    CHECK_EQ(c, 'a');
    CHECK_EQ(shift_held, 0);
    CHECK_EQ(ctrl_held, 0);
}

static void test_ctrl_c_ignores_shift(void) {
    TEST("ctrl+c is decided by the physical key");
    reset_all();
    keyboard_set_foreground(3);

    /* The interrupt check reads the UNSHIFTED table, so Ctrl+Shift+C is an
     * interrupt too. That is the intended reading -- what matters is which
     * physical key is held with Ctrl, not which glyph it would have produced.
     * Pinned because the alternative (comparing against the shifted table)
     * would silently make Ctrl+Shift+C type a capital C into the line instead
     * of stopping the program. */
    feed_scancode(SC_CTRL);
    feed_scancode(SC_LSHIFT);
    feed_scancode(SC_C);

    CHECK_EQ(g_kill_calls, 1);
    CHECK_EQ(g_killed_pid, 3);
    {
        char c = 0;
        CHECK_EQ(keyboard_try_read(&c), 1);
        CHECK_EQ(c, '\3');
    }
}

static void test_ring_buffer_capacity(void) {
    char c = 0;
    int  i;

    TEST("ring buffer capacity");
    reset_all();

    /* One slot is sacrificed to tell full from empty, so the buffer holds
     * SIZE-1 characters. Fill it exactly. */
    for (i = 0; i < KEYBOARD_BUFFER_SIZE - 1; i++) feed_scancode(SC_A);

    /* The next key has nowhere to go and is dropped -- not written over the
     * oldest one, which would reorder the stream. */
    feed_scancode(SC_Z);

    for (i = 0; i < KEYBOARD_BUFFER_SIZE - 1; i++) {
        CHECK_EQ(keyboard_try_read(&c), 1);
        CHECK_EQ(c, 'a');
    }
    CHECK_EQ(keyboard_try_read(&c), 0);    /* the 'z' really was dropped */

    /* Having been full and then fully drained, the buffer must work again --
     * a stuck index would show up here and nowhere else. */
    feed_scancode(SC_Z);
    CHECK_EQ(keyboard_try_read(&c), 1);
    CHECK_EQ(c, 'z');
}

static void test_ring_buffer_wraps(void) {
    char c = 0;
    int  i;

    TEST("ring buffer wrap-around");
    reset_all();

    /* Push several times the buffer's length through it, draining as we go, so
     * both indices cross the modulo boundary repeatedly. Order must hold: this
     * is where a wrap bug shows up as characters arriving transposed rather
     * than as anything crashing. */
    for (i = 0; i < KEYBOARD_BUFFER_SIZE * 3 + 7; i++) {
        uint8_t sc = (i % 2) ? SC_Z : SC_A;

        feed_scancode(sc);
        CHECK_EQ(keyboard_try_read(&c), 1);
        CHECK_EQ(c, (i % 2) ? 'z' : 'a');
    }
    CHECK_EQ(keyboard_try_read(&c), 0);

    /* Interleave differently: half-fill, drain part, refill across the seam. */
    reset_all();
    for (i = 0; i < 100; i++) feed_scancode(SC_A);
    for (i = 0; i < 60; i++) {
        CHECK_EQ(keyboard_try_read(&c), 1);
        CHECK_EQ(c, 'a');
    }
    for (i = 0; i < 80; i++) feed_scancode(SC_Z);   /* wraps past the end */
    for (i = 0; i < 40; i++) {
        CHECK_EQ(keyboard_try_read(&c), 1);
        CHECK_EQ(c, 'a');                            /* the older 40 first */
    }
    for (i = 0; i < 80; i++) {
        CHECK_EQ(keyboard_try_read(&c), 1);
        CHECK_EQ(c, 'z');
    }
    CHECK_EQ(keyboard_try_read(&c), 0);
}

static void test_try_read_arguments(void) {
    TEST("try_read arguments");
    reset_all();

    /* A NULL destination must not consume a character. */
    feed_scancode(SC_A);
    CHECK_EQ(keyboard_try_read(NULL), 0);
    {
        char c = 0;
        CHECK_EQ(keyboard_try_read(&c), 1);
        CHECK_EQ(c, 'a');
    }
}

static void test_blocking_read(void) {
    char buf[8];
    size_t n;

    TEST("blocking read");
    reset_all();

    /* Data already queued: no blocking at all. */
    feed_scancode(SC_A);
    feed_scancode(SC_Z);
    n = keyboard_read(buf, sizeof(buf));
    CHECK_EQ(n, 2);
    CHECK_EQ(buf[0], 'a');
    CHECK_EQ(buf[1], 'z');
    CHECK_EQ(g_block_calls, 0);

    /* Empty buffer: block until a key arrives, then return it. */
    reset_all();
    g_blocks_before_input = 2;
    n = keyboard_read(buf, sizeof(buf));
    CHECK_EQ(n, 1);
    CHECK_EQ(buf[0], 'z');
    CHECK_EQ(g_block_calls, 2);

    /* A spurious wakeup with nothing to read must send the task back to sleep
     * rather than return 0 bytes: callers treat 0 as end of input. */
    CHECK(g_block_calls > 1);

    /* count caps the transfer and leaves the rest queued. */
    reset_all();
    for (int i = 0; i < 5; i++) feed_scancode(SC_A);
    n = keyboard_read(buf, 3);
    CHECK_EQ(n, 3);
    n = keyboard_read(buf, sizeof(buf));
    CHECK_EQ(n, 2);

    /* Degenerate arguments, tested against an EMPTY buffer on purpose.
     *
     * With a character already queued both the correct code and a version that
     * forgot to check `count` return 0, because the transfer loop's own
     * `bytes_read < count` is false either way -- the argument check looks
     * redundant and a missing one is invisible. Empty, the difference is the
     * whole behaviour: the wait loop is reached first, and read(fd, buf, 0) on
     * stdin parks the caller for input it was never going to consume.
     *
     * A key is armed to arrive after one block so a regression fails here with
     * a count mismatch instead of hanging the suite. */
    reset_all();
    g_blocks_before_input = 1;
    CHECK_EQ(keyboard_read(buf, 0), 0);
    CHECK_EQ(g_block_calls, 0);            /* must not wait for input */

    reset_all();
    g_blocks_before_input = 1;
    CHECK_EQ(keyboard_read(NULL, sizeof(buf)), 0);
    CHECK_EQ(g_block_calls, 0);

    /* And with data queued they still return 0 without consuming it. */
    reset_all();
    feed_scancode(SC_A);
    CHECK_EQ(keyboard_read(buf, 0), 0);
    CHECK_EQ(keyboard_read(NULL, sizeof(buf)), 0);
    CHECK_EQ(g_block_calls, 0);
    n = keyboard_read(buf, sizeof(buf));
    CHECK_EQ(n, 1);
    CHECK_EQ(buf[0], 'a');
}

static void test_blocking_read_honours_kill(void) {
    char buf[8];

    TEST("blocking read honours a kill");
    reset_all();
    g_kill_pending = 1;

    /* A task flagged for termination must leave through task_exit() instead of
     * looping in the wait -- with interrupts disabled and nothing left to wake
     * it, looping here would be a kernel that never comes back (F19). */
    if (setjmp(g_exit_jmp) == 0) {
        keyboard_read(buf, sizeof(buf));
        CHECK(0);                          /* must not return normally */
    }
    CHECK_EQ(g_exited, 1);
    CHECK_EQ(g_exit_status, TASK_KILL_STATUS);
}

int main(void) {
    test_install();
    test_basic_mapping();
    test_release_and_dead_keys();
    test_shift_state_machine();
    test_ctrl_c_signals_handler();
    test_ctrl_c_force_kills();
    test_ctrl_c_without_foreground();
    test_ctrl_released_restores_typing();
    test_ctrl_c_ignores_shift();
    test_extended_scancodes();
    test_ring_buffer_capacity();
    test_ring_buffer_wraps();
    test_try_read_arguments();
    test_blocking_read();
    test_blocking_read_honours_kill();
    TEST_REPORT("kb");
}
