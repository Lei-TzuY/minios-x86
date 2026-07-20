#ifndef KB_H
#define KB_H

#include <stddef.h>
#include <stdint.h>

void keyboard_install(void);
int keyboard_try_read(char *c);
size_t keyboard_read(char *buffer, size_t count);

/* Set/clear which process PID receives Ctrl+C kill signals. */
void keyboard_set_foreground(int32_t pid);
void keyboard_clear_foreground(void);

#endif
