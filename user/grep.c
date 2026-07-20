#include "user_syscall.h"

/* grep: copy stdin lines containing a substring to stdout.
 * Usage: grep <pattern>   (typically `... | grep word | ...`). */

#define LINE_MAX 256

/* Return 1 if needle occurs in haystack (empty needle matches everything). */
static int contains(const char *haystack, const char *needle) {
    if (!needle[0]) return 1;
    for (int i = 0; haystack[i]; i++) {
        int j = 0;
        while (needle[j] && haystack[i + j] == needle[j]) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

static void emit_if_match(char *line, int len, const char *pat) {
    line[len] = '\0';
    if (contains(line, pat)) {
        sys_write(line, len);
        sys_write("\n", 1);
    }
}

int main(int argc, char **argv) {
    const char *pat = argc > 1 ? argv[1] : "";
    char line[LINE_MAX];
    char buf[128];
    int len = 0;
    int n;

    while ((n = sys_read(buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || len == LINE_MAX - 1) {
                emit_if_match(line, len, pat);
                len = 0;
            } else {
                line[len++] = c;
            }
        }
    }
    if (len > 0) emit_if_match(line, len, pat);

    return 0;
}
