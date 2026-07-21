#include "vga.h"
#include "io.h"
#include "utils.h"

static inline uint8_t vga_entry_color(enum vga_color fg, enum vga_color bg) {
	return fg | bg << 4;
}

static inline uint16_t vga_entry(unsigned char uc, uint8_t color) {
	return (uint16_t) uc | (uint16_t) color << 8;
}

static const size_t VGA_WIDTH = 80;
static const size_t VGA_HEIGHT = 25;

size_t terminal_row;
size_t terminal_column;
uint8_t terminal_color;
uint16_t* terminal_buffer;

void terminal_clear(void) {
	terminal_row = 0;
	terminal_column = 0;
	for (size_t y = 0; y < VGA_HEIGHT; y++) {
		for (size_t x = 0; x < VGA_WIDTH; x++) {
			const size_t index = y * VGA_WIDTH + x;
			terminal_buffer[index] = vga_entry(' ', terminal_color);
		}
	}
}

void terminal_initialize(void) {
	terminal_color = vga_entry_color(VGA_COLOR_LIGHT_GREY, VGA_COLOR_BLACK);
	terminal_buffer = (uint16_t*) 0xB8000;
	terminal_clear();
}

void terminal_setcolor(uint8_t color) {
	terminal_color = color;
}

void terminal_putentryat(char c, uint8_t color, size_t x, size_t y) {
	const size_t index = y * VGA_WIDTH + x;
	terminal_buffer[index] = vga_entry(c, color);
}

static void terminal_scroll(void) {
	for (size_t y = 1; y < VGA_HEIGHT; y++) {
		for (size_t x = 0; x < VGA_WIDTH; x++) {
			terminal_buffer[(y - 1) * VGA_WIDTH + x] =
				terminal_buffer[y * VGA_WIDTH + x];
		}
	}

	for (size_t x = 0; x < VGA_WIDTH; x++) {
		terminal_buffer[(VGA_HEIGHT - 1) * VGA_WIDTH + x] =
			vga_entry(' ', terminal_color);
	}
	terminal_row = VGA_HEIGHT - 1;
}

void terminal_putchar(char c) {
	outb(0xE9, c); /* QEMU debug console for automated boot verification. */

	if (c == '\b') {
		if (terminal_column > 0) {
			terminal_column--;
			terminal_putentryat(' ', terminal_color, terminal_column, terminal_row);
		}
		return;
	}

	if (c == '\n') {
		terminal_column = 0;
		if (++terminal_row == VGA_HEIGHT) {
			terminal_scroll();
		}
		return;
	}
	terminal_putentryat(c, terminal_color, terminal_column, terminal_row);
	if (++terminal_column == VGA_WIDTH) {
		terminal_column = 0;
		if (++terminal_row == VGA_HEIGHT) {
			terminal_scroll();
		}
	}
}

void terminal_write(const char* data, size_t size) {
	for (size_t i = 0; i < size; i++)
		terminal_putchar(data[i]);
}

void terminal_writestring(const char* data) {
	size_t datalen = 0;
	while (data[datalen] != '\0') datalen++;
	terminal_write(data, datalen);
}

void terminal_write_dec(uint32_t n) {
    /* Format directly as unsigned: int_to_ascii() takes a signed int, and
     * routing a uint32_t through it would reinterpret any value above
     * INT32_MAX as negative (wrong digits), with n == INT32_MIN specifically
     * hitting a signed-overflow negation. Uptime ticks, byte counts and pids
     * are all uint32_t and, in principle, unbounded, so format them without
     * ever converting through a signed type. */
    char str[11];   /* up to 10 digits for UINT32_MAX, no terminator needed */
    int i = 0;

    if (n == 0) {
        terminal_putchar('0');
        return;
    }
    while (n > 0) {
        str[i++] = (char)('0' + n % 10);
        n /= 10;
    }
    while (i > 0) terminal_putchar(str[--i]);
}

/* Print `n` right-justified in `width` columns, zero-padded (e.g. dates). */
void terminal_write_dec_pad(uint32_t n, int width) {
    char buf[16];
    int i = width;

    if (width <= 0 || width >= (int)sizeof(buf)) { terminal_write_dec(n); return; }
    buf[i] = '\0';
    while (i > 0) { buf[--i] = (char)('0' + n % 10); n /= 10; }
    terminal_writestring(buf);
}
