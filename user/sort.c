#include "user_syscall.h"
#include "umalloc.h"
#include "ulib.h"

/* sort: read all of stdin, sort the lines lexicographically, write them out.
 * Input is slurped into a growing buffer; lines are sorted by pointer. */

int main(void) {
    unsigned cap = 512, size = 0;
    char *data = (char *)malloc(cap);
    char buf[128];
    int n, lines = 0, has_partial;
    char **arr;

    if (!data) return 1;

    while ((n = sys_read(buf, sizeof(buf))) > 0) {
        if (size + (unsigned)n + 1 > cap) {
            unsigned ncap = cap * 2;
            char *nd;
            while (size + (unsigned)n + 1 > ncap) ncap *= 2;
            nd = (char *)realloc(data, ncap);
            if (!nd) { free(data); return 1; }
            data = nd;
            cap = ncap;
        }
        umemcpy(data + size, buf, n);
        size += (unsigned)n;
    }

    if (size == 0) { free(data); return 0; }
    data[size] = '\0';  /* terminate a final line that lacks a newline */

    /* Count lines (a trailing partial line still counts). */
    for (unsigned i = 0; i < size; i++) if (data[i] == '\n') lines++;
    has_partial = (data[size - 1] != '\n');
    if (has_partial) lines++;
    if (lines == 0) { free(data); return 0; }

    arr = (char **)malloc((unsigned)lines * sizeof(char *));
    if (!arr) { free(data); return 1; }

    /* Split in place: replace each '\n' with '\0' and record line starts. */
    {
        int li = 0;
        unsigned start = 0;
        for (unsigned i = 0; i < size; i++) {
            if (data[i] == '\n') {
                data[i] = '\0';
                arr[li++] = data + start;
                start = i + 1;
            }
        }
        if (has_partial) arr[li++] = data + start;
    }

    /* Insertion sort (inputs here are small). */
    for (int i = 1; i < lines; i++) {
        char *key = arr[i];
        int j = i - 1;
        while (j >= 0 && ustrcmp(arr[j], key) > 0) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }

    for (int i = 0; i < lines; i++) {
        int l = 0;
        while (arr[i][l]) l++;
        sys_write(arr[i], l);
        sys_write("\n", 1);
    }

    free(arr);
    free(data);
    return 0;
}
