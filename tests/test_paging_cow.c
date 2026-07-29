#include <stddef.h>
#include <stdint.h>

/*
 * Two pure pieces of the paging layer underpin fork():
 *
 *   user_pte(space, vaddr) maps a user virtual address to the page-table entry
 *   backing it, selecting the low 0-4MB table or the 32-36MB mmap table. The
 *   index is a raw shift, so an off-by-one at a region edge would silently read
 *   or write the wrong page -- exactly the kind of corruption that is painful to
 *   chase through a running fork.
 *
 *   The copy-on-write share counts (page_ref[]) record how many EXTRA references
 *   a frame has beyond its sole owner (0 == one owner). cow_ref_release must
 *   return "free me" only when the last reference is gone; a wrong boundary here
 *   frees a frame still shared by another address space, or leaks it forever.
 *
 * Both reach no external function, so --gc-sections drops the rest of paging.c
 * (the fault handler, the asm, pmm/heap) and no stubs are needed.
 */

#include "../paging.c"

/* test.h after paging.c so any SEEK_* enums from the kernel headers precede the
 * SEEK_* macros that test.h's <stdio.h> defines. */
#include "test.h"

/* Real tables for user_pte to point into. */
static page_table_t low_table;
static page_table_t ext_table_mem;

static void test_user_pte_low(void) {
    address_space_t sp = {0};
    sp.first_table = &low_table;
    sp.ext_table = &ext_table_mem;

    TEST("user_pte: low 0-4MB window maps to first_table");
    CHECK(user_pte(&sp, 0x0) == &low_table.pages[0]);
    CHECK(user_pte(&sp, 0xFFF) == &low_table.pages[0]);       /* same page */
    CHECK(user_pte(&sp, 0x1000) == &low_table.pages[1]);
    CHECK(user_pte(&sp, 0x3FF000) == &low_table.pages[1023]); /* last page <4MB */
    CHECK(user_pte(&sp, 0x3FFFFF) == &low_table.pages[1023]);
}

static void test_user_pte_ext(void) {
    address_space_t sp = {0};
    sp.first_table = &low_table;
    sp.ext_table = &ext_table_mem;

    TEST("user_pte: 32-36MB mmap window maps to ext_table");
    CHECK(user_pte(&sp, USER_EXT_BASE) == &ext_table_mem.pages[0]);
    CHECK(user_pte(&sp, USER_EXT_BASE + 0x1000) == &ext_table_mem.pages[1]);
    CHECK(user_pte(&sp, USER_EXT_TOP - 0x1000) == &ext_table_mem.pages[1023]);
}

static void test_user_pte_gaps(void) {
    address_space_t sp = {0};
    sp.first_table = &low_table;
    sp.ext_table = &ext_table_mem;

    TEST("user_pte: addresses outside both windows return NULL");
    CHECK(user_pte(&sp, 0x400000) == NULL);            /* exactly 4MB: the gap */
    CHECK(user_pte(&sp, USER_EXT_BASE - 0x1000) == NULL);
    CHECK(user_pte(&sp, USER_EXT_TOP) == NULL);        /* exactly the top */
    CHECK(user_pte(&sp, USER_EXT_TOP + 0x1000) == NULL);

    TEST("user_pte: NULL space or absent table returns NULL");
    CHECK(user_pte(NULL, 0x1000) == NULL);
    sp.first_table = NULL;
    CHECK(user_pte(&sp, 0x1000) == NULL);              /* low addr, no table */
    sp.ext_table = NULL;
    CHECK(user_pte(&sp, USER_EXT_BASE) == NULL);       /* ext addr, no table */
}

static void test_cow_inc(void) {
    uint32_t f = 5;
    page_ref[f] = 0;

    TEST("cow_ref_inc counts extra references");
    cow_ref_inc(f);
    CHECK_EQ(page_ref[f], 1);
    cow_ref_inc(f);
    CHECK_EQ(page_ref[f], 2);

    TEST("cow_ref_inc ignores an out-of-range frame");
    cow_ref_inc(FRAME_COUNT);          /* must not write past the array */
    cow_ref_inc(FRAME_COUNT + 100);
    page_ref[FRAME_COUNT - 1] = 0;
    cow_ref_inc(FRAME_COUNT - 1);      /* highest valid frame still counts */
    CHECK_EQ(page_ref[FRAME_COUNT - 1], 1);
}

static void test_cow_release(void) {
    uint32_t f = 7;

    TEST("cow_ref_release frees only when the last reference is gone");
    page_ref[f] = 0;                   /* sole owner */
    CHECK_EQ(cow_ref_release(f), 1);   /* last reference -> free */
    CHECK_EQ(page_ref[f], 0);

    page_ref[f] = 2;                   /* three owners total */
    CHECK_EQ(cow_ref_release(f), 0);   /* still shared -> keep */
    CHECK_EQ(page_ref[f], 1);
    CHECK_EQ(cow_ref_release(f), 0);   /* still shared -> keep */
    CHECK_EQ(page_ref[f], 0);
    CHECK_EQ(cow_ref_release(f), 1);   /* now the last -> free */
    CHECK_EQ(page_ref[f], 0);          /* stays pinned at zero, no underflow */

    TEST("cow_ref_release treats an out-of-range frame as freeable");
    CHECK_EQ(cow_ref_release(FRAME_COUNT), 1);
}

static void test_cow_fork_sequence(void) {
    uint32_t f = 42;
    page_ref[f] = 0;                   /* parent owns the frame */

    TEST("cow refcount tracks a fork/exit sequence");
    cow_ref_inc(f);                    /* fork: child shares */
    cow_ref_inc(f);                    /* fork again: grandchild shares */
    CHECK_EQ(page_ref[f], 2);          /* three sharers, 2 extra */

    CHECK_EQ(cow_ref_release(f), 0);   /* one exits */
    CHECK_EQ(cow_ref_release(f), 0);   /* another exits */
    CHECK_EQ(cow_ref_release(f), 1);   /* last one exits -> frame freed */
}

int main(void) {
    test_user_pte_low();
    test_user_pte_ext();
    test_user_pte_gaps();
    test_cow_inc();
    test_cow_release();
    test_cow_fork_sequence();
    TEST_REPORT("paging-cow");
}
