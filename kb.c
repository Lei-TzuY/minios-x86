#include "kb.h"
#include "isr.h"
#include "io.h"
#include "irq.h"
#include "process.h"
#include "task.h"
#include "vga.h"

#define KEYBOARD_BUFFER_SIZE 128

/* US Keyboard Layout Scancode table */
unsigned char kbdus[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', /* 9 */
  '9', '0', '-', '=', '\b', /* Backspace */
  '\t',         /* Tab */
  'q', 'w', 'e', 'r', /* 19 */
  't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n', /* Enter key */
    0,          /* 29   - Control */
  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', /* 39 */
 '\'', '`',   0,        /* Left shift */
 '\\', 'z', 'x', 'c', 'v', 'b', 'n',            /* 49 */
  'm', ',', '.', '/',   0,              /* Right shift */
  '*',
    0,  /* Alt */
  ' ',  /* Space bar */
    0,  /* Caps lock */
    0,  /* 59 - F1 key ... > */
    0,   0,   0,   0,   0,   0,   0,   0,
    0,  /* < ... F10 */
    0,  /* 69 - Num lock*/
    0,  /* Scroll Lock */
    0,  /* Home key */
    0,  /* Up Arrow */
    0,  /* Page Up */
  '-',
    0,  /* Left Arrow */
    0,
    0,  /* Right Arrow */
  '+',
    0,  /* 79 - End key*/
    0,  /* Down Arrow */
    0,  /* Page Down */
    0,  /* Insert Key */
    0,  /* Delete Key */
    0,   0,   0,
    0,  /* F11 Key */
    0,  /* F12 Key */
    0,  /* All other keys are undefined */
};

/* Shifted variant of the US layout (same scancode indices as kbdus). */
unsigned char kbdus_shift[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', /* 9 */
  '(', ')', '_', '+', '\b', /* Backspace */
  '\t',         /* Tab */
  'Q', 'W', 'E', 'R', /* 19 */
  'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n', /* Enter key */
    0,          /* 29   - Control */
  'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', /* 39 */
  '"', '~',   0,        /* Left shift */
  '|', 'Z', 'X', 'C', 'V', 'B', 'N',            /* 49 */
  'M', '<', '>', '?',   0,              /* Right shift */
  '*',
    0,  /* Alt */
  ' ',  /* Space bar */
    0,  /* Caps lock */
    0,  /* 59 - F1 key ... > */
    0,   0,   0,   0,   0,   0,   0,   0,
    0,  /* < ... F10 */
    0,  /* 69 - Num lock*/
    0,  /* Scroll Lock */
    0,  /* Home key */
    0,  /* Up Arrow */
    0,  /* Page Up */
  '-',
    0,  /* Left Arrow */
    0,
    0,  /* Right Arrow */
  '+',
    0,  /* 79 - End key*/
    0,  /* Down Arrow */
    0,  /* Page Down */
    0,  /* Insert Key */
    0,  /* Delete Key */
    0,   0,   0,
    0,  /* F11 Key */
    0,  /* F12 Key */
    0,  /* All other keys are undefined */
};

static char keyboard_buffer[KEYBOARD_BUFFER_SIZE];
static volatile size_t keyboard_read_index = 0;
static volatile size_t keyboard_write_index = 0;
static volatile uint8_t ctrl_held = 0;
static volatile uint8_t shift_held = 0;
static volatile int32_t kb_foreground_pid = -1;

void keyboard_set_foreground(int32_t pid) { kb_foreground_pid = pid; }
void keyboard_clear_foreground(void)       { kb_foreground_pid = -1; }

static void keyboard_buffer_write(char c) {
    size_t next_index = (keyboard_write_index + 1) % KEYBOARD_BUFFER_SIZE;

    if (next_index == keyboard_read_index) {
        return; /* Drop input when the buffer is full. */
    }

    keyboard_buffer[keyboard_write_index] = c;
    keyboard_write_index = next_index;
}

static void keyboard_callback(registers_t *regs) {
    (void)regs;
    
    uint8_t scancode = inb(0x60);
    
    /* If the top bit of the byte we read from the PRT is set, that
     * means that a key has just been released */
    if (scancode & 0x80) {
        /* Key release: track modifier state */
        uint8_t code = scancode & 0x7F;
        if (code == 0x1D) ctrl_held = 0;
        else if (code == 0x2A || code == 0x36) shift_held = 0;
    } else if (scancode == 0x1D) {
        /* Left Ctrl pressed */
        ctrl_held = 1;
    } else if (scancode == 0x2A || scancode == 0x36) {
        /* Left/Right Shift pressed */
        shift_held = 1;
    } else if (ctrl_held && kbdus[scancode] == 'c') {
        /* Ctrl+C: deliver SIGINT to the foreground process. If it installed a
         * handler the signal is catchable; otherwise force-kill it. */
        terminal_writestring("^C\n");
        keyboard_buffer_write('\3');
        task_wake_one(keyboard_buffer);
        if (kb_foreground_pid >= 0) {
            if (process_has_sighandler(kb_foreground_pid, SIGINT))
                process_send_signal(kb_foreground_pid, SIGINT);
            else
                process_request_kill(kb_foreground_pid);
        }
    } else {
        char c = shift_held ? kbdus_shift[scancode] : kbdus[scancode];
        if (c) {
            keyboard_buffer_write(c);
            task_wake_one(keyboard_buffer);
            terminal_putchar(c);
        }
    }
}

int keyboard_try_read(char *c) {
    uint32_t flags;

    if (!c) return 0;

    flags = save_irq_disable();
    if (keyboard_read_index == keyboard_write_index) {
        restore_irq(flags);
        return 0;
    }

    *c = keyboard_buffer[keyboard_read_index];
    keyboard_read_index = (keyboard_read_index + 1) % KEYBOARD_BUFFER_SIZE;
    restore_irq(flags);
    return 1;
}

size_t keyboard_read(char *buffer, size_t count) {
    size_t bytes_read = 0;
    uint32_t flags;

    if (!buffer || count == 0) return 0;

    flags = save_irq_disable();
    while (keyboard_read_index == keyboard_write_index) {
        task_block_killable(keyboard_buffer);
    }

    while (bytes_read < count && keyboard_read_index != keyboard_write_index) {
        buffer[bytes_read] = keyboard_buffer[keyboard_read_index];
        keyboard_read_index = (keyboard_read_index + 1) % KEYBOARD_BUFFER_SIZE;
        bytes_read++;
    }

    restore_irq(flags);
    return bytes_read;
}

void keyboard_install(void) {
    /* IRQ1 is keyboard */
    register_interrupt_handler(33, keyboard_callback);
}
