#include "user_syscall.h"
#include "ulib.h"

/* head: copy the first N lines of stdin to stdout (default 10).
 * Usage: head [count]   (e.g. `... | head 3`). */

#define LINE_MAX 256

int main(int argc, char **argv) {
    int limit = argc > 1 ? uatoi(argv[1]) : 10;
    char line[LINE_MAX];
    char buf[128];
    int len = 0, lines = 0, n;

    if (limit <= 0) return 0;

    while ((n = sys_read(buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (len < LINE_MAX) line[len++] = c;
            if (c == '\n') {
                sys_write(line, len);
                len = 0;
                if (++lines >= limit) return 0;
            }
        }
    }
    if (len > 0 && lines < limit) sys_write(line, len);
    return 0;
}
