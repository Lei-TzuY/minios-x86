#include "user_syscall.h"
#include "umalloc.h"

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

static int fail(const char *why) {
    write_str("[malloc test failed: ");
    write_str(why);
    write_str("]\n");
    return 1;
}

int main(void) {
    /* A: basic alloc, write a byte pattern, read it back, free. */
    unsigned char *a = (unsigned char *)malloc(100);
    if (!a) return fail("basic alloc");
    for (int i = 0; i < 100; i++) a[i] = (unsigned char)(i * 7 + 1);
    for (int i = 0; i < 100; i++)
        if (a[i] != (unsigned char)(i * 7 + 1)) return fail("basic verify");
    free(a);

    /* B: many independent blocks with distinct contents, all verified. */
    unsigned char *blocks[64];
    for (int i = 0; i < 64; i++) {
        unsigned sz = (unsigned)(8 + i * 13);
        blocks[i] = (unsigned char *)malloc(sz);
        if (!blocks[i]) return fail("multi alloc");
        for (unsigned j = 0; j < sz; j++)
            blocks[i][j] = (unsigned char)(i ^ j);
    }
    for (int i = 0; i < 64; i++) {
        unsigned sz = (unsigned)(8 + i * 13);
        for (unsigned j = 0; j < sz; j++)
            if (blocks[i][j] != (unsigned char)(i ^ j))
                return fail("multi verify");
    }
    /* Free every other block, then re-allocate to exercise the free list. */
    for (int i = 0; i < 64; i += 2) free(blocks[i]);
    for (int i = 0; i < 64; i += 2) {
        blocks[i] = (unsigned char *)malloc(32);
        if (!blocks[i]) return fail("reuse alloc");
    }
    for (int i = 0; i < 64; i++) free(blocks[i]);

    /* C: calloc returns zeroed memory. */
    unsigned int *z = (unsigned int *)calloc(50, sizeof(unsigned int));
    if (!z) return fail("calloc");
    for (int i = 0; i < 50; i++)
        if (z[i] != 0) return fail("calloc zero");
    free(z);

    /* D: realloc grows a block while preserving its contents. */
    char *s = (char *)malloc(8);
    if (!s) return fail("realloc base");
    s[0] = 'm'; s[1] = 'i'; s[2] = 'n'; s[3] = 'i';
    s[4] = 'O'; s[5] = 'S'; s[6] = '\0';
    s = (char *)realloc(s, 64);
    if (!s) return fail("realloc grow");
    if (s[0] != 'm' || s[3] != 'i' || s[5] != 'S' || s[6] != '\0')
        return fail("realloc preserve");
    free(s);

    /* E: a large block that forces several pages of sbrk growth. */
    unsigned char *big = (unsigned char *)malloc(20000);
    if (!big) return fail("large alloc");
    big[0] = 0xAB;
    big[19999] = 0xCD;
    if (big[0] != 0xAB || big[19999] != 0xCD) return fail("large verify");
    free(big);

    /* F: bounded exhaustion — fill the heap, confirm graceful NULL, recover. */
    void *chunks[160];
    int n = 0;
    while (n < 160) {
        void *c = malloc(4000);
        if (!c) break;
        chunks[n++] = c;
    }
    if (n == 0) return fail("exhaustion alloc");
    for (int i = 0; i < n; i++) free(chunks[i]);
    /* After freeing everything a fresh large alloc must succeed again. */
    void *again = malloc(8000);
    if (!again) return fail("recover after exhaustion");
    free(again);

    write_str("[malloc test passed] allocations before full: ");
    write_int(n);
    write_str("\n");
    return 0;
}
