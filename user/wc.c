#include "user_syscall.h"

/* Count lines, words and characters from standard input until EOF.
 * Output format matches Unix wc: "<lines> <words> <chars>".
 *
 * With no redirection, stdin is the keyboard (interactive); typically run as
 * `wc < file` so the kernel feeds file bytes and returns 0 (EOF) at the end. */
int main(void) {
    char buf[128];
    int lines = 0, words = 0, chars = 0;
    int in_word = 0;
    int n;

    while ((n = sys_read(buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            chars++;
            if (c == '\n') lines++;
            if (c == ' ' || c == '\t' || c == '\n') {
                in_word = 0;
            } else if (!in_word) {
                in_word = 1;
                words++;
            }
        }
    }

    write_int(lines);
    sys_write(" ", 1);
    write_int(words);
    sys_write(" ", 1);
    write_int(chars);
    sys_write("\n", 1);
    return 0;
}
