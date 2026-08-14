#include "test.h"

/*
 * The VGA text console (vga.c): cursor tracking, line wrap, scrolling, and the
 * two decimal formatters.
 *
 * Everything the kernel prints goes through terminal_putchar, and the QEMU
 * suite's several hundred assertions are all greps over what came out of it --
 * yet nothing has ever checked the console's own logic. The suite reads the
 * debug-console byte stream (port 0xE9), which putchar emits BEFORE any of the
 * cursor arithmetic runs, so every scroll, wrap and backspace bug is invisible
 * to it: the bytes still arrive in order while the screen shows nonsense.
 *
 * What that leaves untested is exactly the interesting part. Scrolling copies
 * 24 rows and blanks one; the column wrap and the newline path each have their
 * own copy of the "did we fall off the bottom" check; backspace decrements a
 * size_t that would wrap to four billion if it ever went below zero, and the
 * next write would land that far outside the frame buffer.
 *
 * Two substitutions make this run on the host, neither of which touches a
 * kernel header (the CAP15 method):
 *
 *   - io.h is replaced by pre-defining its include guard, so outb() can be
 *     captured. That is not only about avoiding a privileged instruction: the
 *     port-0xE9 write IS the interface the QEMU suite consumes, so it deserves
 *     assertions of its own.
 *   - terminal_buffer is a global, so it is pointed at an ordinary array. The
 *     one thing that cannot run hosted is terminal_initialize(), whose whole
 *     body is the assignment of the hardware address 0xB8000; the test sets up
 *     the same state by hand and exercises everything downstream of it.
 */

/* --- port I/O: replace io.h ------------------------------------------------ */
#define IO_H
#include <stdint.h>
#include <stddef.h>

#define DEBUG_LOG_MAX 4096
static char     g_debug_log[DEBUG_LOG_MAX];
static unsigned g_debug_len;
static uint16_t g_last_port;
static unsigned g_outb_calls;

static inline void outb(uint16_t port, uint8_t val) {
    g_outb_calls++;
    g_last_port = port;
    if (g_debug_len < DEBUG_LOG_MAX - 1) g_debug_log[g_debug_len++] = (char)val;
    g_debug_log[g_debug_len] = '\0';
}
static inline uint8_t inb(uint16_t port) { (void)port; return 0; }
static inline uint16_t inw(uint16_t port) { (void)port; return 0; }
static inline void outw(uint16_t port, uint16_t val) { (void)port; (void)val; }
static inline void io_wait(void) { }

#include "../vga.c"

/* --- fake frame buffer ---------------------------------------------------- */

#define CELLS (80 * 25)
#define SLACK 96
#define GUARD 0xDEAD

/* The frame sits in the middle of a larger array, with guard cells on BOTH
 * sides. Trailing slack alone is not enough: scrolling reads the row above
 * the one it writes, so a loop that starts one row early reads before the
 * start of the frame. With the frame at index 0 that is a wild read the
 * test can only discover by crashing; with a guarded prefix it is an
 * assertion that names what happened. */
static uint16_t fake_backing[SLACK + CELLS + SLACK];
static uint16_t *const fake_vga = fake_backing + SLACK;

static void reset_console(void) {
    for (unsigned i = 0; i < SLACK + CELLS + SLACK; i++)
        fake_backing[i] = GUARD;
    terminal_buffer = fake_vga;
    terminal_color = vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
    terminal_clear();
    g_debug_len = 0;
    g_debug_log[0] = '\0';
    g_outb_calls = 0;
}

static char cell_char(size_t x, size_t y) {
    return (char)(fake_vga[y * VGA_WIDTH + x] & 0xFF);
}

static uint8_t cell_color(size_t x, size_t y) {
    return (uint8_t)(fake_vga[y * VGA_WIDTH + x] >> 8);
}

/* Nothing may be written outside the 80x25 frame, on either side. */
static int slack_intact(void) {
    for (unsigned i = 0; i < SLACK; i++)
        if (fake_backing[i] != GUARD) return 0;
    for (unsigned i = 0; i < SLACK; i++)
        if (fake_vga[CELLS + i] != GUARD) return 0;
    return 1;
}

/* --- tests ---------------------------------------------------------------- */

static void test_clear(void) {
    TEST("clear");
    reset_console();

    CHECK_EQ(terminal_row, 0);
    CHECK_EQ(terminal_column, 0);
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        for (size_t x = 0; x < VGA_WIDTH; x++) {
            CHECK_EQ(cell_char(x, y), ' ');
            CHECK_EQ(cell_color(x, y), terminal_color);
        }
    }
    CHECK(slack_intact());

    /* Clearing from a moved cursor puts it back at the origin. */
    terminal_writestring("some text\nmore");
    CHECK(terminal_row != 0 || terminal_column != 0);
    terminal_clear();
    CHECK_EQ(terminal_row, 0);
    CHECK_EQ(terminal_column, 0);
    CHECK_EQ(cell_char(0, 0), ' ');

    /* The blank it fills with carries the CURRENT colour, not the boot one. */
    terminal_setcolor(vga_entry_color(VGA_COLOR_RED, VGA_COLOR_BLUE));
    terminal_clear();
    CHECK_EQ(cell_color(0, 0), vga_entry_color(VGA_COLOR_RED, VGA_COLOR_BLUE));
    CHECK_EQ(cell_color(79, 24), vga_entry_color(VGA_COLOR_RED, VGA_COLOR_BLUE));
}

static void test_putentryat(void) {
    TEST("putentryat");
    reset_console();

    /* The index is y * WIDTH + x. Getting the two the wrong way round still
     * writes inside the buffer for small values, so check a cell where the
     * transposition would land somewhere else entirely. */
    terminal_putentryat('A', 0x1F, 3, 2);
    CHECK_EQ(cell_char(3, 2), 'A');
    CHECK_EQ(cell_color(3, 2), 0x1F);
    CHECK_EQ(cell_char(2, 3), ' ');        /* not the transposed cell */

    terminal_putentryat('Z', 0x4E, 79, 24);   /* the last cell */
    CHECK_EQ(cell_char(79, 24), 'Z');
    CHECK(slack_intact());

    /* It does not move the cursor: that is putchar's job. */
    CHECK_EQ(terminal_row, 0);
    CHECK_EQ(terminal_column, 0);
}

static void test_plain_characters(void) {
    TEST("plain characters");
    reset_console();

    terminal_putchar('h');
    CHECK_EQ(cell_char(0, 0), 'h');
    CHECK_EQ(terminal_column, 1);
    CHECK_EQ(terminal_row, 0);

    terminal_putchar('i');
    CHECK_EQ(cell_char(1, 0), 'i');
    CHECK_EQ(terminal_column, 2);

    /* Tab gets no special handling -- it is placed as an ordinary glyph and
     * advances one column. The keyboard does deliver '\t' (scancode 0x0F maps
     * to it), so this is a reachable path, and it is pinned here as the
     * current behaviour rather than assumed to be tab expansion. */
    reset_console();
    terminal_putchar('\t');
    CHECK_EQ(cell_char(0, 0), '\t');
    CHECK_EQ(terminal_column, 1);
    CHECK_EQ(terminal_row, 0);

    /* Every character also goes to the debug console, which is the byte stream
     * the QEMU suite greps. It is emitted for control characters too. */
    reset_console();
    terminal_putchar('a');
    terminal_putchar('\n');
    terminal_putchar('\b');
    CHECK_EQ(g_debug_len, 3);
    CHECK_EQ(g_debug_log[0], 'a');
    CHECK_EQ(g_debug_log[1], '\n');
    CHECK_EQ(g_debug_log[2], '\b');
    CHECK_EQ(g_last_port, 0xE9);
}

static void test_newline(void) {
    TEST("newline");
    reset_console();

    terminal_writestring("ab");
    terminal_putchar('\n');
    CHECK_EQ(terminal_column, 0);
    CHECK_EQ(terminal_row, 1);

    /* A newline does not blank the rest of the line it left. */
    CHECK_EQ(cell_char(0, 0), 'a');
    CHECK_EQ(cell_char(1, 0), 'b');
    CHECK_EQ(cell_char(2, 0), ' ');

    terminal_putchar('c');
    CHECK_EQ(cell_char(0, 1), 'c');

    /* Consecutive newlines each advance a row. */
    reset_console();
    terminal_putchar('\n');
    terminal_putchar('\n');
    terminal_putchar('\n');
    CHECK_EQ(terminal_row, 3);
    CHECK_EQ(terminal_column, 0);
}

static void test_backspace(void) {
    TEST("backspace");
    reset_console();

    terminal_writestring("abc");
    CHECK_EQ(terminal_column, 3);
    terminal_putchar('\b');
    CHECK_EQ(terminal_column, 2);
    CHECK_EQ(cell_char(2, 0), ' ');        /* the cell is blanked, not just left */
    CHECK_EQ(cell_char(1, 0), 'b');        /* and only that one */

    /* Backspace at column 0 is a no-op. terminal_column is a size_t, so a
     * decrement here would wrap to SIZE_MAX and the next character would be
     * written four billion cells away -- the guard is load-bearing, not
     * cosmetic, and the slack check below is what would notice. */
    reset_console();
    CHECK_EQ(terminal_column, 0);
    terminal_putchar('\b');
    CHECK_EQ(terminal_column, 0);
    CHECK_EQ(terminal_row, 0);
    terminal_putchar('x');
    CHECK_EQ(cell_char(0, 0), 'x');
    CHECK(slack_intact());

    /* It does not walk back onto the previous line either. */
    reset_console();
    terminal_writestring("ab\n");
    CHECK_EQ(terminal_row, 1);
    terminal_putchar('\b');
    CHECK_EQ(terminal_row, 1);
    CHECK_EQ(terminal_column, 0);
    CHECK_EQ(cell_char(1, 0), 'b');        /* the previous line is untouched */
}

static void test_column_wrap(void) {
    TEST("column wrap");
    reset_console();

    for (size_t i = 0; i < VGA_WIDTH; i++) terminal_putchar('x');
    /* The 80th character fills the last column and the cursor moves to the
     * start of the next row -- it does not sit at column 80. */
    CHECK_EQ(cell_char(VGA_WIDTH - 1, 0), 'x');
    CHECK_EQ(terminal_column, 0);
    CHECK_EQ(terminal_row, 1);
    CHECK(slack_intact());

    terminal_putchar('y');
    CHECK_EQ(cell_char(0, 1), 'y');
    CHECK_EQ(terminal_column, 1);
}

static void test_scroll_on_newline(void) {
    char marker;

    TEST("scroll (newline)");
    reset_console();
    CHECK(slack_intact());

    /* Put a distinct character at the start of each of the 25 rows. */
    for (size_t y = 0; y < VGA_HEIGHT; y++) {
        terminal_putchar((char)('A' + (int)y));
        if (y + 1 < VGA_HEIGHT) terminal_putchar('\n');
    }
    CHECK_EQ(terminal_row, VGA_HEIGHT - 1);
    CHECK_EQ(cell_char(0, 0), 'A');
    CHECK_EQ(cell_char(0, VGA_HEIGHT - 1), (char)('A' + (int)VGA_HEIGHT - 1));

    /* One more newline scrolls: every row moves up by one, the top is lost,
     * and the cursor stays on the last row rather than running off the end. */
    terminal_putchar('\n');
    CHECK_EQ(terminal_row, VGA_HEIGHT - 1);
    CHECK_EQ(terminal_column, 0);

    for (size_t y = 0; y + 1 < VGA_HEIGHT; y++) {
        marker = (char)('A' + (int)y + 1);
        CHECK_EQ(cell_char(0, y), marker);
    }

    /* The vacated bottom row is blanked in the current colour, not left
     * holding a copy of what used to be there. */
    for (size_t x = 0; x < VGA_WIDTH; x++) {
        CHECK_EQ(cell_char(x, VGA_HEIGHT - 1), ' ');
        CHECK_EQ(cell_color(x, VGA_HEIGHT - 1), terminal_color);
    }
    CHECK(slack_intact());
}

static void test_scroll_on_wrap(void) {
    TEST("scroll (column wrap)");
    reset_console();

    /* The wrap path has its own copy of the bottom-of-screen check, so reach
     * the scroll by filling the last line rather than by a newline. */
    for (size_t y = 0; y + 1 < VGA_HEIGHT; y++) {
        terminal_putchar((char)('a' + (int)y));
        terminal_putchar('\n');
    }
    CHECK_EQ(terminal_row, VGA_HEIGHT - 1);

    for (size_t i = 0; i < VGA_WIDTH; i++) terminal_putchar('#');
    CHECK_EQ(terminal_row, VGA_HEIGHT - 1);   /* scrolled, not row 25 */
    CHECK_EQ(terminal_column, 0);
    CHECK(slack_intact());

    /* The row of '#' moved up one, and the bottom row is blank. */
    for (size_t x = 0; x < VGA_WIDTH; x++)
        CHECK_EQ(cell_char(x, VGA_HEIGHT - 2), '#');
    CHECK_EQ(cell_char(0, VGA_HEIGHT - 1), ' ');
    CHECK_EQ(cell_char(0, 0), 'b');           /* 'a' was scrolled off */
}

static void test_repeated_scrolling(void) {
    TEST("repeated scrolling");
    reset_console();

    /* Far more lines than the screen holds. The invariant that matters is that
     * the cursor never leaves the frame: if terminal_row ever exceeded
     * VGA_HEIGHT the "== VGA_HEIGHT" test would stop matching and every
     * subsequent write would land outside the buffer. */
    for (int i = 0; i < 200; i++) {
        terminal_putchar((char)('0' + (i % 10)));
        terminal_putchar('\n');
        CHECK(terminal_row < VGA_HEIGHT);
        CHECK(terminal_column < VGA_WIDTH);
    }
    CHECK(slack_intact());

    /* The last ten lines written are the ones on screen. */
    CHECK_EQ(cell_char(0, VGA_HEIGHT - 2), '9');
}

static void test_write_and_writestring(void) {
    TEST("write / writestring");
    reset_console();

    terminal_write("hello", 5);
    CHECK_EQ(cell_char(0, 0), 'h');
    CHECK_EQ(cell_char(4, 0), 'o');
    CHECK_EQ(terminal_column, 5);

    /* write() honours the count and ignores what follows, including a NUL. */
    reset_console();
    terminal_write("ab\0cd", 5);
    CHECK_EQ(cell_char(0, 0), 'a');
    CHECK_EQ(cell_char(1, 0), 'b');
    CHECK_EQ(cell_char(2, 0), '\0');
    CHECK_EQ(cell_char(3, 0), 'c');
    CHECK_EQ(terminal_column, 5);

    /* writestring stops at the NUL. */
    reset_console();
    terminal_writestring("ab");
    CHECK_EQ(terminal_column, 2);

    /* Zero-length and empty inputs write nothing at all. */
    reset_console();
    terminal_write("anything", 0);
    CHECK_EQ(terminal_column, 0);
    CHECK_EQ(cell_char(0, 0), ' ');
    terminal_writestring("");
    CHECK_EQ(terminal_column, 0);
    CHECK_EQ(g_debug_len, 0);
}

static void test_write_dec(void) {
    TEST("write_dec");

    reset_console();
    terminal_write_dec(0);
    CHECK_STREQ(g_debug_log, "0");

    reset_console();
    terminal_write_dec(7);
    CHECK_STREQ(g_debug_log, "7");

    reset_console();
    terminal_write_dec(10);
    CHECK_STREQ(g_debug_log, "10");

    reset_console();
    terminal_write_dec(12345);
    CHECK_STREQ(g_debug_log, "12345");

    /* The reason this function exists: values above INT32_MAX must format as
     * unsigned. Routing them through the signed int_to_ascii() would print a
     * negative number, and INT32_MIN would hit a signed-overflow negation. */
    reset_console();
    terminal_write_dec(2147483648u);
    CHECK_STREQ(g_debug_log, "2147483648");

    reset_console();
    terminal_write_dec(4294967295u);
    CHECK_STREQ(g_debug_log, "4294967295");

    /* Ten digits is the widest it produces, which is what the /proc
     * generators reserve per numeric field. */
    CHECK_EQ(g_debug_len, 10);
}

static void test_write_dec_pad(void) {
    TEST("write_dec_pad");

    reset_console();
    terminal_write_dec_pad(7, 2);
    CHECK_STREQ(g_debug_log, "07");

    reset_console();
    terminal_write_dec_pad(2026, 4);
    CHECK_STREQ(g_debug_log, "2026");

    reset_console();
    terminal_write_dec_pad(0, 3);
    CHECK_STREQ(g_debug_log, "000");

    /* More digits than the field: the high ones are dropped, keeping the field
     * width. That is what a date formatter wants (a two-digit month field
     * stays two wide) and it is the current behaviour either way. */
    reset_console();
    terminal_write_dec_pad(123, 2);
    CHECK_STREQ(g_debug_log, "23");

    /* Degenerate widths fall back to the unpadded formatter rather than
     * indexing the local buffer out of range: buf is 16 bytes and `width` is
     * used as an index into it. */
    reset_console();
    terminal_write_dec_pad(42, 0);
    CHECK_STREQ(g_debug_log, "42");

    reset_console();
    terminal_write_dec_pad(42, -1);
    CHECK_STREQ(g_debug_log, "42");

    reset_console();
    terminal_write_dec_pad(42, 16);
    CHECK_STREQ(g_debug_log, "42");

    reset_console();
    terminal_write_dec_pad(42, 15);        /* the widest field that is allowed */
    CHECK_STREQ(g_debug_log, "000000000000042");
    CHECK_EQ(g_debug_len, 15);
}

static void test_color(void) {
    TEST("colour");
    reset_console();

    CHECK_EQ(vga_entry_color(VGA_COLOR_WHITE, VGA_COLOR_BLUE), 0x1F);
    CHECK_EQ(vga_entry('A', 0x1F), 0x1F41);

    terminal_setcolor(vga_entry_color(VGA_COLOR_LIGHT_GREEN, VGA_COLOR_BLACK));
    terminal_putchar('g');
    CHECK_EQ(cell_color(0, 0), vga_entry_color(VGA_COLOR_LIGHT_GREEN,
                                               VGA_COLOR_BLACK));

    /* A colour change applies from then on, not retroactively. */
    terminal_setcolor(vga_entry_color(VGA_COLOR_RED, VGA_COLOR_BLACK));
    terminal_putchar('r');
    CHECK_EQ(cell_color(0, 0), vga_entry_color(VGA_COLOR_LIGHT_GREEN,
                                               VGA_COLOR_BLACK));
    CHECK_EQ(cell_color(1, 0), vga_entry_color(VGA_COLOR_RED,
                                               VGA_COLOR_BLACK));
}

int main(void) {
    test_clear();
    test_putentryat();
    test_plain_characters();
    test_newline();
    test_backspace();
    test_column_wrap();
    test_scroll_on_newline();
    test_scroll_on_wrap();
    test_repeated_scrolling();
    test_write_and_writestring();
    test_write_dec();
    test_write_dec_pad();
    test_color();
    TEST_REPORT("vga");
}
