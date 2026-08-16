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

/*
 * The stub hands out pages from one contiguous arena, in address order.
 *
 * That is deliberate, and it matters. posix_memalign returns unrelated regions
 * whose relative addresses are up to the host allocator, which quietly makes
 * two properties untestable: whether successive heap regions coalesce when
 * they ARE contiguous, and whether the free list stays sorted by address so
 * that adjacency can be detected at all. The real pmm_alloc_blocks hands back
 * consecutive frames, so contiguous growth is the normal case in the kernel,
 * not an exotic one.
 *
 * g_stub_gap_pages forces a gap before the next region, for the one test that
 * needs two regions that are genuinely not adjacent.
 */
#define ARENA_PAGES 64
static unsigned char arena[ARENA_PAGES * PMM_BLOCK_SIZE]
    __attribute__((aligned(PMM_BLOCK_SIZE)));
static uint32_t arena_next;
static uint32_t g_stub_gap_pages;
static int stub_alloc_calls;
/* The bytes the stub deliberately skipped: memory the heap was never
 * given and must never hand out. */
static unsigned char *g_gap_start;
static unsigned char *g_gap_end;

void *pmm_alloc_blocks(uint32_t count) {
    unsigned char *p;

    stub_alloc_calls++;
    if (count == 0) return NULL;

    if (g_stub_gap_pages) {
        g_gap_start = arena + (size_t)arena_next * PMM_BLOCK_SIZE;
        arena_next += g_stub_gap_pages;
        g_gap_end = arena + (size_t)arena_next * PMM_BLOCK_SIZE;
        g_stub_gap_pages = 0;
    }
    if (arena_next + count > ARENA_PAGES) return NULL;

    p = arena + (size_t)arena_next * PMM_BLOCK_SIZE;
    arena_next += count;
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


/* The block header sits immediately before the payload kmalloc hands back. */
static heap_block_t *header_of(void *payload) {
    return (heap_block_t *)((unsigned char *)payload - sizeof(heap_block_t));
}

static void test_split_threshold(void) {
    unsigned char *host;
    unsigned char *small;
    size_t host_size;

    /*
     * kmalloc splits a block only when the leftover would be worth having:
     *
     *     if (best_fit->size > size + sizeof(heap_block_t) + 4)
     *
     * That comparison is the classic place for an off-by-one, and its two
     * outcomes are hard to tell apart from the outside -- both return a
     * correctly sized pointer. What differs is the block that is left behind:
     * split too eagerly and the remainder is a header describing a payload of
     * zero or fewer bytes, which then sits in the free list forever and, at
     * exactly zero, is adjacent to nothing it can merge with.
     *
     * So drive the request to land on each side of the threshold and check the
     * SERVING block's recorded size, which is the only thing that says whether
     * a split happened.
     */
    TEST("split threshold");

    /* Take a large block, then free it so its exact size is known and it is
     * the first thing a subsequent request will be served from. */
    host = kmalloc(1024);
    CHECK(host != NULL);
    if (!host) return;
    kfree(host);
    /* Read the size AFTER freeing: kfree coalesces the block with the free
     * remainder that follows it, so the span the next request will be served
     * from is larger than the one that was handed out. Measuring before the
     * free would put both cases below on the same side of the threshold. */
    host_size = header_of(host)->size;

    /* Ask for everything except a header plus four bytes: the leftover would
     * be a zero-byte payload, so the block must be handed over whole. */
    {
        size_t exact = host_size - sizeof(heap_block_t) - 4;

        small = kmalloc(exact);
        CHECK(small != NULL);
        if (!small) return;
        CHECK_EQ((void *)small, (void *)host);          /* same block */
        CHECK_EQ(header_of(small)->size, host_size);    /* NOT split */
        kfree(small);
    }

    /* One aligned step smaller and the leftover is worth a block, so the
     * server must shrink to exactly the requested size. Checking both sides
     * is what stops "never split" from passing. */
    {
        size_t splittable = host_size - sizeof(heap_block_t) - 8;

        small = kmalloc(splittable);
        CHECK(small != NULL);
        if (!small) return;
        CHECK_EQ((void *)small, (void *)host);
        CHECK_EQ(header_of(small)->size, splittable);   /* split happened */

        /* And the remainder is a usable free block, not a zero-length stub. */
        {
            heap_block_t *rest = header_of(small)->next;

            CHECK(rest != NULL);
            if (rest) {
                CHECK_EQ(rest->is_free, 1);
                CHECK(rest->size > 0);
            }
        }
        kfree(small);
    }
}

static void test_non_adjacent_blocks_do_not_merge(void) {
    unsigned char *a, *b, *c;
    size_t a_size;

    /*
     * Coalescing must check that two free blocks are physically adjacent, not
     * merely neighbours in the list. The existing coalescing test frees three
     * blocks that ARE adjacent, so it passes whether or not the adjacency test
     * exists; this is the negative case that isolates it.
     *
     * With the middle block still in use, the first and third are neighbours
     * in the free list after the middle one is skipped -- and merging them
     * would swallow the live allocation between them.
     */
    TEST("non-adjacent free blocks stay separate");
    a = kmalloc(256);
    b = kmalloc(256);
    c = kmalloc(256);
    CHECK(a && b && c);
    if (!a || !b || !c) return;

    for (int i = 0; i < 256; i++) b[i] = (unsigned char)(i ^ 0x5A);

    a_size = header_of(a)->size;
    kfree(a);
    kfree(c);

    /* `a` must still describe only itself: swallowing `b` would show up as a
     * size covering the live block. */
    CHECK_EQ(header_of(a)->size, a_size);
    CHECK_EQ(header_of(b)->is_free, 0);

    /* And the live allocation is intact. */
    for (int i = 0; i < 256; i++) CHECK_EQ(b[i], (unsigned char)(i ^ 0x5A));

    /* A request that only fits in the merged span must therefore NOT be
     * served from `a`. */
    {
        unsigned char *big = kmalloc(a_size + 64);

        CHECK(big != NULL);
        CHECK((void *)big != (void *)a);
        kfree(big);
    }

    kfree(b);
}

static void test_free_order_independence(void) {
    unsigned char *p[4];
    size_t before, after;

    /*
     * merge_free_blocks walks the list once per kfree and merges a run of
     * adjacent free blocks by staying put after each merge. Freeing in the
     * middle first exercises that chaining from a different starting point;
     * freeing back-to-front exercises it from the other end. Both must end
     * with the same single span.
     */
    TEST("coalescing is independent of free order");

    for (int i = 0; i < 4; i++) {
        p[i] = kmalloc(128);
        CHECK(p[i] != NULL);
        if (!p[i]) return;
    }
    before = heap_get_free_bytes();
    kfree(p[1]);
    kfree(p[3]);
    kfree(p[0]);
    kfree(p[2]);
    after = heap_get_free_bytes();
    CHECK(after > before);

    /* All four spans plus the three headers between them are now one block, so
     * a request larger than 4 * 128 must fit where they were. */
    {
        unsigned char *whole = kmalloc(4 * 128 + 2 * sizeof(heap_block_t));

        CHECK(whole != NULL);
        CHECK_EQ((void *)whole, (void *)p[0]);
        kfree(whole);
    }
}


/* Snapshot of the free list, so a test can say what changed and why. */
#define SNAP_MAX 64
static heap_block_t *snap_block[SNAP_MAX];
static size_t        snap_size[SNAP_MAX];
static int           snap_count;

static void snapshot_list(void) {
    heap_block_t *b = heap_first_block();

    snap_count = 0;
    while (b && snap_count < SNAP_MAX) {
        snap_block[snap_count] = b;
        snap_size[snap_count] = b->size;
        snap_count++;
        b = b->next;
    }
}

/*
 * Every block that grew must have done so by absorbing a run of blocks that
 * were each physically contiguous with the one before it. That is the whole
 * safety condition for coalescing: a block whose span grew to cover anything
 * else is now handing out memory it does not own, which is corruption rather
 * than a lost optimisation.
 *
 * Checking only the immediate successor is not enough, and getting that wrong
 * is how the first version of this helper let two real mutants through. When
 * three blocks merge, only the absorbing block's header is updated -- the
 * absorbed ones keep their old size and next, sitting as stale bytes inside
 * the survivor's payload. So a snapshot taken beforehand shows exactly one
 * changed size no matter how many blocks were swallowed, and the growth has to
 * be reconciled against the whole run to say whether it was legitimate.
 */
static int growth_only_by_adjacency(void) {
    for (int i = 0; i < snap_count; i++) {
        heap_block_t *b = snap_block[i];
        size_t before = snap_size[i];
        unsigned char *new_end;
        unsigned char *run_end;
        int k;

        if (b->size == before) continue;
        if (b->size < before) return 0;             /* nothing may shrink */

        new_end = (unsigned char *)b + sizeof(heap_block_t) + b->size;
        run_end = (unsigned char *)b + sizeof(heap_block_t) + before;

        for (k = i + 1; k < snap_count; k++) {
            /* The next block absorbed must start exactly where the run so far
             * ended. A gap here means memory that was never part of the heap
             * has just been folded into a free block. */
            if ((unsigned char *)snap_block[k] != run_end) return 0;
            run_end = (unsigned char *)snap_block[k]
                      + sizeof(heap_block_t) + snap_size[k];
            if (run_end == new_end) break;
            if (run_end > new_end) return 0;        /* overshot: bad arithmetic */
        }
        if (k >= snap_count) return 0;              /* growth never accounted for */
    }
    return 1;
}

/* No block may claim any part of the hole the stub left. This is stated
 * against the arena rather than against a snapshot of the list, so it
 * holds regardless of what earlier tests left behind -- and it says the
 * property directly: the heap must never hand out memory it was not
 * given. */
static int no_block_spans_the_gap(void) {
    heap_block_t *b = heap_first_block();

    if (!g_gap_start) return 1;             /* no gap was ever made */
    while (b) {
        unsigned char *start = (unsigned char *)b;
        unsigned char *end = start + sizeof(heap_block_t) + b->size;

        if (start < g_gap_end && end > g_gap_start) return 0;
        b = b->next;
    }
    return 1;
}

static void test_alignment(void) {
    unsigned char *p[4];

    /*
     * kmalloc rounds the request up to HEAP_ALIGNMENT before doing anything
     * else. Every size the rest of this suite uses is already a multiple of
     * four, so removing the rounding changes nothing they can see -- yet the
     * split arithmetic and every pointer handed to the kernel depend on it.
     */
    TEST("alignment");

    for (int i = 0; i < 4; i++) {
        p[i] = kmalloc(5 + (size_t)i);          /* 5, 6, 7, 8 */
        CHECK(p[i] != NULL);
        if (!p[i]) return;
        CHECK_EQ(((uintptr_t)p[i]) % 4, 0);     /* payload stays aligned */
    }

    /* 5, 6, 7 all round up to 8; 8 stays 8. */
    CHECK_EQ(header_of(p[0])->size, 8);
    CHECK_EQ(header_of(p[1])->size, 8);
    CHECK_EQ(header_of(p[2])->size, 8);
    CHECK_EQ(header_of(p[3])->size, 8);

    /* A one-byte request still yields an aligned, usable block. */
    {
        unsigned char *one = kmalloc(1);

        CHECK(one != NULL);
        if (one) {
            CHECK_EQ(((uintptr_t)one) % 4, 0);
            CHECK_EQ(header_of(one)->size, 4);
            one[0] = 0xAB;
            CHECK_EQ(one[0], 0xAB);
            kfree(one);
        }
    }

    for (int i = 0; i < 4; i++) kfree(p[i]);
}

static void test_split_remainder_fits(void) {
    unsigned char *host;
    unsigned char *served;
    heap_block_t *rest;
    size_t host_size;

    /*
     * The remainder left by a split must describe exactly the space it owns:
     *
     *     new_block->size = best_fit->size - size - sizeof(heap_block_t)
     *
     * Forgetting the header term makes the remainder claim twelve bytes more
     * than it has, so the next allocation served from it runs into whatever
     * follows. The serving block looks perfect either way, which is why the
     * check has to be on the remainder's span rather than on the pointer.
     */
    TEST("split remainder describes only its own space");

    host = kmalloc(1024);
    CHECK(host != NULL);
    if (!host) return;
    kfree(host);
    host_size = header_of(host)->size;      /* after coalescing */

    served = kmalloc(64);
    CHECK(served != NULL);
    if (!served) return;
    rest = header_of(served)->next;
    CHECK(rest != NULL);
    if (!rest) return;

    /* The remainder starts where the served block ends ... */
    CHECK_EQ((void *)((unsigned char *)header_of(served)
                      + sizeof(heap_block_t) + header_of(served)->size),
             (void *)rest);

    /* ... and is exactly the space left over: the original payload minus
     * what was served and minus the header the remainder itself needs.
     * Stated as equality rather than as "does not overrun the next
     * block", because the remainder is often the last block in the list,
     * and then there is no next block for an overrun to collide with --
     * the claim is still wrong, just not yet fatal. */
    CHECK_EQ(rest->size, host_size - header_of(served)->size
                         - sizeof(heap_block_t));
    if (rest->next) {
        CHECK((unsigned char *)rest + sizeof(heap_block_t) + rest->size
              <= (unsigned char *)rest->next);
    }

    kfree(served);
}

static void test_contiguous_growth_merges(void) {
    heap_block_t *b;
    heap_block_t *tail = NULL;
    size_t largest = 0;
    size_t tail_size;
    int calls_before;
    unsigned char *big;

    /*
     * When the heap grows, the new region is inserted into the free list in
     * address order and then coalesced. Both halves of that matter, and only
     * together: the real pmm hands back consecutive frames, so a new region
     * usually begins exactly where the previous one ended, and merging the two
     * is what lets a request larger than either half be served at all.
     *
     * Insert at the head instead of in order and nothing breaks outright --
     * every allocation still succeeds, every free still coalesces within its
     * own region. What is lost is the join across the boundary, so the heap
     * fragments at every growth. That is invisible to a test that only checks
     * that allocations succeed, which is why this one asks for something that
     * fits ONLY in the joined span.
     */
    TEST("a contiguous new region joins the free tail");

    for (b = heap_first_block(); b; b = b->next) {
        if (b->is_free && b->size > largest) largest = b->size;
        tail = b;
    }
    CHECK(tail != NULL);
    if (!tail) return;
    CHECK_EQ(tail->is_free, 1);
    tail_size = tail->size;

    /* Bigger than any single existing block, so the heap has to grow, and the
     * new pages start exactly where the tail ends. */
    calls_before = stub_alloc_calls;
    big = kmalloc(largest + PMM_BLOCK_SIZE);
    CHECK(big != NULL);
    CHECK(stub_alloc_calls > calls_before);
    if (!big) return;

    /* Served from the tail itself. Without the join it would have to come from
     * the new region alone, at a higher address. */
    CHECK_EQ((void *)header_of(big), (void *)tail);
    CHECK(header_of(big)->size > tail_size);

    /* And the payload is genuinely usable across the boundary. */
    {
        size_t n = largest + PMM_BLOCK_SIZE;

        for (size_t i = 0; i < n; i += 512) big[i] = (unsigned char)(i / 512);
        for (size_t i = 0; i < n; i += 512)
            CHECK_EQ(big[i], (unsigned char)(i / 512));
    }
    kfree(big);
}

static void test_regions_with_a_gap_never_merge(void) {
    unsigned char *a, *b;
    int calls_before;

    /*
     * Two free blocks that are neighbours in the list are not necessarily
     * neighbours in memory. When the heap grows twice and the regions are not
     * contiguous, merging on list order alone hands out the gap between them
     * -- memory the heap never owned.
     *
     * Every other coalescing test in this file frees blocks carved from one
     * region, where list order and physical order agree, so the adjacency test
     * could be deleted without any of them noticing. The stub is told to leave
     * a gap so this one can tell the difference.
     */
    TEST("regions separated by a gap never merge");

    a = kmalloc(PMM_BLOCK_SIZE * 2);
    CHECK(a != NULL);
    if (!a) return;

    calls_before = stub_alloc_calls;
    g_stub_gap_pages = 2;                       /* leave a hole in the arena */
    b = kmalloc(PMM_BLOCK_SIZE * 2);
    CHECK(b != NULL);
    CHECK(stub_alloc_calls > calls_before);     /* a second region was taken */
    if (!b) return;

    CHECK(g_gap_start != NULL);             /* the hole really was made */
    CHECK(no_block_spans_the_gap());

    snapshot_list();
    kfree(b);
    CHECK(growth_only_by_adjacency());
    CHECK(no_block_spans_the_gap());

    snapshot_list();
    kfree(a);
    CHECK(growth_only_by_adjacency());
    CHECK(no_block_spans_the_gap());
}

static void test_growth_is_always_adjacent(void) {
    unsigned char *p[6];

    /* The same invariant over ordinary traffic: free four of six blocks in a
     * scattered order and check after every step. */
    TEST("coalescing never absorbs a non-neighbour");

    for (int i = 0; i < 6; i++) {
        p[i] = kmalloc(96);
        CHECK(p[i] != NULL);
        if (!p[i]) return;
    }

    {
        static const int order[] = { 2, 5, 0, 3, 1, 4 };

        for (int k = 0; k < 6; k++) {
            snapshot_list();
            kfree(p[order[k]]);
            CHECK(growth_only_by_adjacency());
        }
    }
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
    test_split_threshold();
    test_non_adjacent_blocks_do_not_merge();
    test_free_order_independence();
    test_alignment();
    test_split_remainder_fits();
    test_growth_is_always_adjacent();
    test_contiguous_growth_merges();
    test_regions_with_a_gap_never_merge();
    test_multi_page_allocation();
    test_free_null_is_safe();
    CHECK(stub_alloc_calls > 0);   /* the heap really did grow via pmm */
    TEST_REPORT("heap");
}
