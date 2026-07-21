#include "test.h"
#include "../heap.h"
#include "../pmm.h"

#include <stdlib.h>

/*
 * The kernel heap actually dereferences the memory it gets from the physical
 * allocator, so the real pmm (which returns frame numbers as fake pointers)
 * cannot back it here. Stub pmm_alloc_blocks with page-aligned host memory
 * instead -- heap.c calls nothing else from pmm.
 *
 * What this pins down: splitting a block, reusing a freed one, and coalescing
 * adjacent free blocks. A coalescing bug shows up in the running kernel only
 * as gradual fragmentation, which no end-to-end assertion would ever catch.
 */

static int stub_alloc_calls;

void *pmm_alloc_blocks(uint32_t count) {
    void *p = NULL;
    stub_alloc_calls++;
    if (count == 0) return NULL;
    if (posix_memalign(&p, PMM_BLOCK_SIZE, (size_t)count * PMM_BLOCK_SIZE) != 0)
        return NULL;
    return p;
}

static void test_basic_alloc(void) {
    unsigned char *a;

    TEST("basic alloc");
    CHECK(kmalloc(0) == NULL);

    a = kmalloc(64);
    CHECK(a != NULL);
    /* The payload must be writable across its full length. */
    for (int i = 0; i < 64; i++) a[i] = (unsigned char)i;
    for (int i = 0; i < 64; i++) CHECK_EQ(a[i], (unsigned char)i);
    kfree(a);
}

static void test_blocks_do_not_overlap(void) {
    unsigned char *a, *b, *c;

    TEST("no overlap");
    a = kmalloc(100);
    b = kmalloc(100);
    c = kmalloc(100);
    CHECK(a && b && c);

    /* Fill each with a distinct pattern, then verify none was disturbed --
     * that is what a bad split (payload overlapping the next header) breaks. */
    for (int i = 0; i < 100; i++) { a[i] = 0xAA; b[i] = 0xBB; c[i] = 0xCC; }
    for (int i = 0; i < 100; i++) {
        CHECK_EQ(a[i], 0xAA);
        CHECK_EQ(b[i], 0xBB);
        CHECK_EQ(c[i], 0xCC);
    }
    kfree(a); kfree(b); kfree(c);
}

static void test_reuse_after_free(void) {
    void *a, *b;

    TEST("reuse after free");
    a = kmalloc(128);
    CHECK(a != NULL);
    kfree(a);
    b = kmalloc(128);
    /* The same-sized request right after a free should land in that hole
     * rather than growing the heap. */
    CHECK_EQ(b, a);
    kfree(b);
}

static void test_coalescing(void) {
    void *a, *b, *c, *big;
    size_t free_before, free_after;

    TEST("coalescing");
    a = kmalloc(256);
    b = kmalloc(256);
    c = kmalloc(256);
    CHECK(a && b && c);

    free_before = heap_get_free_bytes();
    kfree(a);
    kfree(b);
    kfree(c);
    free_after = heap_get_free_bytes();
    CHECK(free_after > free_before);

    /* Three adjacent 256-byte holes must merge into one span large enough for
     * a single request bigger than any of them individually. */
    big = kmalloc(700);
    CHECK(big != NULL);
    CHECK_EQ(big, a);          /* merged starting at the lowest block */
    kfree(big);
}

static void test_multi_page_allocation(void) {
    unsigned char *p;
    size_t big = PMM_BLOCK_SIZE * 2 + 512;

    TEST("multi-page alloc");
    p = kmalloc(big);
    CHECK(p != NULL);
    /* Touch both ends to confirm the whole span is really backed. */
    p[0] = 0x11;
    p[big - 1] = 0x22;
    CHECK_EQ(p[0], 0x11);
    CHECK_EQ(p[big - 1], 0x22);
    kfree(p);
}

static void test_free_null_is_safe(void) {
    TEST("free(NULL)");
    kfree(NULL);          /* must not crash */
    CHECK(1);
}

int main(void) {
    heap_init();
    test_basic_alloc();
    test_blocks_do_not_overlap();
    test_reuse_after_free();
    test_coalescing();
    test_multi_page_allocation();
    test_free_null_is_safe();
    CHECK(stub_alloc_calls > 0);   /* the heap really did grow via pmm */
    TEST_REPORT("heap");
}
