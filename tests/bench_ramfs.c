#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

/*
 * Quantify the RAMFS geometric-growth change. This measures algorithmic work,
 * not wall-clock time, so the result is exact and independent of host, CPU and
 * compiler: it counts how many times the file buffer is reallocated and how
 * many bytes are copied purely because of growth while a file is built up by
 * many small appends.
 *
 * Only ramfs.c is linked; every libc-ish dependency it has is provided here in
 * a counting form, so kmalloc calls and memcpy bytes are observed directly
 * rather than inferred.
 *
 * The pre-change code reallocated and copied the whole file on every extending
 * write, so its cost is exact and needs no code to measure -- it is shown as a
 * closed form beside the measured new numbers. For N appends of K bytes:
 *   reallocations    old = N            new = measured (~log2 of the size)
 *   growth-copy bytes old = K*N*(N-1)/2 new = measured (~final size)
 */

/* --- counting stand-ins for the functions ramfs.c calls ------------------- */
static long g_reallocs;       /* kmalloc calls during the measured window */
static long g_memcpy_bytes;   /* bytes moved by memcpy during that window */
static int  g_count;          /* window on/off */

void *kmalloc(size_t size) {
    if (g_count) g_reallocs++;
    return malloc(size ? size : 1);
}
void kfree(void *p) { free(p); }

void *memcpy(void *d, const void *s, size_t n) {
    if (g_count) g_memcpy_bytes += (long)n;
    unsigned char *dd = d; const unsigned char *ss = s;
    for (size_t i = 0; i < n; i++) dd[i] = ss[i];
    return d;
}
void *memset(void *d, int v, size_t n) {
    unsigned char *dd = d;
    for (size_t i = 0; i < n; i++) dd[i] = (unsigned char)v;
    return d;
}
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) ; return r; }

/* --- the module under test ------------------------------------------------ */
#include "../fs.h"
#include "../ramfs.h"

/* ramfs.c reads fs_root (defined in fs.c, which we do not link). */
fs_node_t *fs_root = NULL;

static void measure(long N, long K) {
    char payload[64];
    fs_node_t *f;
    long off, i;

    for (i = 0; i < K; i++) payload[i] = (char)('a' + (i % 26));

    /* Fresh filesystem each run so files never collide. */
    fs_root = NULL;
    ramfs_init();
    f = ramfs_create_file("f");
    if (!f) { printf("  create failed\n"); return; }

    g_reallocs = 0;
    g_memcpy_bytes = 0;
    g_count = 1;                       /* start measuring at the first append */

    off = 0;
    for (i = 0; i < N; i++) {
        f->write(f, (uint32_t)off, (uint32_t)K, (uint8_t *)payload);
        off += K;
    }
    g_count = 0;

    /* The payload copy (N*K bytes) happens in both algorithms; the growth copy
     * is the overhead that changed. */
    long payload_bytes = N * K;
    long growth_bytes = g_memcpy_bytes - payload_bytes;
    if (growth_bytes < 0) growth_bytes = 0;

    long old_reallocs = N;
    long long old_growth = (long long)K * N * (N - 1) / 2;

    printf("  N=%-5ld K=%-3ld | reallocs: new=%-4ld old=%-6ld"
           " | growth-copy bytes: new=%-8ld old=%-12lld  (%.0fx less)\n",
           N, K, g_reallocs, old_reallocs, growth_bytes, old_growth,
           growth_bytes > 0 ? (double)old_growth / growth_bytes : 0.0);
}

int main(void) {
    printf("RAMFS append cost -- building a file with N writes of K bytes\n");
    printf("(exact algorithmic counts, host-independent)\n\n");
    measure(64, 16);
    measure(128, 16);
    measure(256, 16);
    measure(512, 16);
    measure(1024, 8);
    printf("\nold = reallocate+copy the whole file on every extending write"
           " (the pre-change behaviour, shown as its exact closed form).\n");
    printf("Reallocations drop from N to about log2(N*K/64); growth copying"
           " drops from O(N^2) to O(final size).\n");
    return 0;
}
