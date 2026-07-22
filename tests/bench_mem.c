#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>

/*
 * Measure the kernel's word-at-a-time memcpy/memset against a plain
 * byte-at-a-time baseline, at the sizes that actually dominate on the kernel
 * hot path: a 512-byte ATA sector, a 4 KB page (every demand-paged/COW frame
 * and every heap grow is zeroed one of these), plus small sizes where the
 * setup cost of the fast path could in principle hurt.
 *
 * The word version under test is the real utils.c code, linked in, so this
 * measures what the kernel ships rather than a re-implementation.
 *
 * HONEST LIMITS -- read before quoting a number:
 *   - This runs on the x86-64 host under gcc -O1, not the kernel's 32-bit
 *     -O2 -ffreestanding build. Treat the RATIO and its direction as the
 *     result, not the absolute ns.
 *   - A ratio near 1.0 at small/unaligned sizes is itself a finding: it says
 *     the fast path does not regress the cases it cannot speed up.
 * The point of this file is to replace an unmeasured performance claim with
 * evidence of the right order of magnitude, not to produce a spec sheet.
 */

/* The word version: the kernel's own implementations. */
extern void *memset(void *dest, int val, size_t len);
extern void *memcpy(void *dest, const void *src, size_t len);

/* The byte-at-a-time baseline, i.e. what the code looked like before. */
static void *byte_memset(void *dest, int val, size_t len) {
    uint8_t *p = (uint8_t *)dest;
    while (len-- > 0) *p++ = (uint8_t)val;
    return dest;
}
static void *byte_memcpy(void *dest, const void *src, size_t len) {
    uint8_t *d = (uint8_t *)dest;
    const uint8_t *s = (const uint8_t *)src;
    while (len-- > 0) *d++ = *s++;
    return dest;
}

static uint8_t bufa[8192];
static uint8_t bufb[8192];
static volatile uint8_t sink;   /* defeats dead-store elimination */

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e9 + ts.tv_nsec;
}

/* Return the best (minimum) per-call time in ns over several rounds; the
 * minimum is the measurement least polluted by scheduler/cache noise. */
typedef void *(*memset_fn)(void *, int, size_t);
typedef void *(*memcpy_fn)(void *, const void *, size_t);

static double best_memset(memset_fn fn, uint8_t *dst, size_t len) {
    double best = 1e30;
    for (int round = 0; round < 7; round++) {
        long iters = 2000000 / (long)(len + 4) + 50;
        double t0 = now_ns();
        for (long i = 0; i < iters; i++) {
            fn(dst, (int)i, len);
            sink ^= dst[len ? len - 1 : 0];
        }
        double per = (now_ns() - t0) / (double)iters;
        if (per < best) best = per;
    }
    return best;
}

static double best_memcpy(memcpy_fn fn, uint8_t *dst, const uint8_t *src, size_t len) {
    double best = 1e30;
    for (int round = 0; round < 7; round++) {
        long iters = 2000000 / (long)(len + 4) + 50;
        double t0 = now_ns();
        for (long i = 0; i < iters; i++) {
            fn(dst, src, len);
            sink ^= dst[len ? len - 1 : 0];
        }
        double per = (now_ns() - t0) / (double)iters;
        if (per < best) best = per;
    }
    return best;
}

static void row_set(const char *label, uint8_t *dst, size_t len) {
    double b = best_memset(byte_memset, dst, len);
    double w = best_memset(memset, dst, len);
    printf("  %-22s byte=%8.1f ns  word=%8.1f ns  speedup=%.2fx\n",
           label, b, w, b / w);
}
static void row_cpy(const char *label, uint8_t *dst, const uint8_t *src, size_t len) {
    double b = best_memcpy(byte_memcpy, dst, src, len);
    double w = best_memcpy(memcpy, dst, src, len);
    printf("  %-22s byte=%8.1f ns  word=%8.1f ns  speedup=%.2fx\n",
           label, b, w, b / w);
}

int main(void) {
    printf("memset (host x86-64, gcc -O1; treat the ratio as the result):\n");
    row_set("4096 aligned (page)", bufa, 4096);
    row_set("512 aligned (sector)", bufa, 512);
    row_set("64 aligned", bufa, 64);
    row_set("4096 unaligned+1", bufa + 1, 4096);
    row_set("7 bytes", bufa, 7);

    printf("memcpy (host x86-64, gcc -O1; treat the ratio as the result):\n");
    row_cpy("4096 aligned (page)", bufa, bufb, 4096);
    row_cpy("512 aligned (sector)", bufa, bufb, 512);
    row_cpy("64 aligned", bufa, bufb, 64);
    row_cpy("4096 dst+1 (unaligned)", bufa + 1, bufb, 4096);
    row_cpy("4096 dst+1/src+3", bufa + 1, bufb + 3, 4096);
    row_cpy("7 bytes", bufa, bufb, 7);

    printf("(sink=%d)\n", sink);   /* keep the compiler honest */
    return 0;
}
