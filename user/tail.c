#include "user_syscall.h"
#include "umalloc.h"
#include "ulib.h"

/* tail: copy the last N lines of stdin to stdout (default 10).
 * Lines are buffered in a malloc'd ring of size N. Usage: tail [count]. */

#define LINE_MAX 256

int main(int argc, char **argv) {
    int limit = argc > 1 ? uatoi(argv[1]) : 10;
    char **ring;
    int *lens;
    char line[LINE_MAX];
    char buf[128];
    int len = 0, n;
    int head = 0, count = 0;

    if (limit <= 0) return 0;

    ring = (char **)calloc(limit, sizeof(char *));
    lens = (int *)calloc(limit, sizeof(int));
    if (!ring || !lens) return 1;

#define STORE()                                                  \
    do {                                                         \
        if (ring[head]) free(ring[head]);                        \
        ring[head] = (char *)malloc(len ? (unsigned)len : 1u);   \
        if (ring[head]) { umemcpy(ring[head], line, len);        \
                          lens[head] = len; }                    \
        head = (head + 1) % limit;                               \
        count++;                                                 \
    } while (0)

    while ((n = sys_read(buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (len < LINE_MAX) line[len++] = c;
            if (c == '\n') { STORE(); len = 0; }
        }
    }
    if (len > 0) STORE();

    int total = count < limit ? count : limit;
    int start = (count <= limit) ? 0 : head;  /* oldest kept line */
    for (int i = 0; i < total; i++) {
        int idx = (start + i) % limit;
        if (ring[idx]) sys_write(ring[idx], lens[idx]);
    }
    return 0;
}
