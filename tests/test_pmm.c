#include "test.h"
#include "../pmm.h"

/*
 * The physical memory manager hands out frame numbers as fake pointers and
 * never dereferences them, so it runs unmodified on the host. Its bitmap
 * arithmetic, the "block 0 is never handed out" rule, and the rejection of
 * malformed frees are all things the running kernel would only reveal as a
 * mysterious corruption much later.
 */

#define REGION_BASE  0x100000u          /* 1 MB */
#define REGION_SIZE  (256u * PMM_BLOCK_SIZE)
#define MEM_KB       (4u * 1024u)       /* 4 MB of managed memory */

static void reset_pmm(void) {
    pmm_init(MEM_KB);
    pmm_init_region(REGION_BASE, REGION_SIZE);
}

static void test_init_accounting(void) {
    TEST("init accounting");
    reset_pmm();
    CHECK_EQ(pmm_get_total_blocks(), MEM_KB / 4);
    /* Everything starts reserved; only the freed region is available. */
    CHECK_EQ(pmm_get_free_blocks(), 256);
    CHECK_EQ(pmm_get_used_blocks(),
             pmm_get_total_blocks() - pmm_get_free_blocks());
}

static void test_alloc_free_roundtrip(void) {
    void *a, *b;

    TEST("alloc/free roundtrip");
    reset_pmm();

    a = pmm_alloc_block();
    CHECK(a != NULL);
    CHECK_EQ((unsigned long)a % PMM_BLOCK_SIZE, 0);
    CHECK_EQ(pmm_get_free_blocks(), 255);

    b = pmm_alloc_block();
    CHECK(b != NULL);
    CHECK(b != a);
    CHECK_EQ(pmm_get_free_blocks(), 254);

    pmm_free_block(a);
    pmm_free_block(b);
    CHECK_EQ(pmm_get_free_blocks(), 256);
}

static void test_never_returns_block_zero(void) {
    /* Frame 0 must stay reserved so a successful allocation never looks like
     * NULL to the caller. */
    TEST("block 0 reserved");
    pmm_init(MEM_KB);
    pmm_init_region(0, REGION_SIZE);      /* deliberately include frame 0 */

    for (int i = 0; i < 32; i++) {
        void *p = pmm_alloc_block();
        CHECK(p != NULL);
    }
}

static void test_free_count_is_honest(void) {
    /* Reserving frame 0 has to be reflected in the accounting, not just in the
     * bitmap. A region that starts at 0 has frame 0 released by the loop (which
     * decrements used_blocks) and then re-reserved afterwards; if the count is
     * not restored, the manager advertises one more free block than it can
     * actually hand out. Allocating until exhaustion is the honest measure. */
    uint32_t reported;
    int got = 0;

    TEST("free count is honest");
    pmm_init(MEM_KB);
    pmm_init_region(0, REGION_SIZE);      /* region includes frame 0 */

    reported = pmm_get_free_blocks();
    while (pmm_alloc_block() != NULL) {
        got++;
        if (got > 1000) break;
    }
    CHECK_EQ(got, reported);
    CHECK_EQ(pmm_get_free_blocks(), 0);
}

static void test_contiguous_run(void) {
    unsigned long first;

    TEST("contiguous run");
    reset_pmm();

    first = (unsigned long)pmm_alloc_blocks(4);
    CHECK(first != 0);
    CHECK_EQ(pmm_get_free_blocks(), 252);
    /* A run must be genuinely contiguous. */
    pmm_free_blocks((void *)first, 4);
    CHECK_EQ(pmm_get_free_blocks(), 256);

    TEST("run too large");
    CHECK(pmm_alloc_blocks(1000000u) == NULL);
    CHECK(pmm_alloc_blocks(0) == NULL);
    CHECK_EQ(pmm_get_free_blocks(), 256);
}

static void test_rejects_bad_frees(void) {
    void *p;

    TEST("bad frees ignored");
    reset_pmm();
    p = pmm_alloc_block();
    CHECK(p != NULL);
    CHECK_EQ(pmm_get_free_blocks(), 255);

    pmm_free_block(NULL);                      /* NULL */
    pmm_free_blocks((void *)0x12345, 1);       /* unaligned */
    pmm_free_blocks(p, 0);                     /* zero count */
    pmm_free_blocks((void *)0xF0000000u, 1);   /* beyond managed memory */
    CHECK_EQ(pmm_get_free_blocks(), 255);      /* none of them counted */

    /* Freeing the same block twice must not inflate the free count. */
    pmm_free_block(p);
    CHECK_EQ(pmm_get_free_blocks(), 256);
    pmm_free_block(p);
    CHECK_EQ(pmm_get_free_blocks(), 256);
}

static void test_exhaustion(void) {
    int got = 0;

    TEST("exhaustion");
    reset_pmm();
    while (pmm_alloc_block() != NULL) {
        got++;
        if (got > 1000) break;             /* guard against a runaway loop */
    }
    CHECK_EQ(got, 256);
    CHECK_EQ(pmm_get_free_blocks(), 0);
    CHECK(pmm_alloc_block() == NULL);      /* stays NULL once empty */
}

int main(void) {
    test_init_accounting();
    test_alloc_free_roundtrip();
    test_never_returns_block_zero();
    test_free_count_is_honest();
    test_contiguous_run();
    test_rejects_bad_frees();
    test_exhaustion();
    TEST_REPORT("pmm");
}
