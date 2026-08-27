#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>

/*
 * The user-pointer validators are the kernel's front line: sys_write and
 * friends run them on every raw pointer a ring-3 program hands in. The bugs
 * fixed earlier (F2, F14) were exactly missing bounds checks of this kind, and
 * the security-critical case -- a pointer near the top of the user range with a
 * huge length, which must not be let through by integer overflow -- cannot be
 * driven from the shell.
 *
 * user_buffer_valid does not dereference the pointer (only range arithmetic
 * plus paging_user_range_mapped), so it is tested with fabricated addresses and
 * a stubbed mapping oracle. user_string_valid does dereference, so it needs
 * real memory at the user addresses; that is obtained with MAP_FIXED_NOREPLACE
 * and those cases are skipped if the mapping cannot be placed.
 *
 * syscall.c is large and coupled; --gc-sections drops everything the test does
 * not call, so paging_user_range_mapped is the only stub needed.
 */

/* Mapping oracle: report [g_lo, g_hi) as mapped, or everything when g_yes is
 * set. g_yes isolates the bound arithmetic in user_buffer_valid from the
 * mapping check, so an overflow in the bound cannot be masked by the oracle
 * independently rejecting a huge range. */
static uint32_t g_lo, g_hi;
static int g_yes;
int paging_user_range_mapped(uint32_t vaddr, uint32_t size) {
    if (g_yes) return 1;
    if (size == 0) return 1;
    if (vaddr < g_lo || vaddr >= g_hi) return 0;
    return (uint64_t)vaddr + size <= (uint64_t)g_hi;
}

#include "../syscall.c"

#include "test.h"

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static void map_all(void) { g_yes = 0; g_lo = USER_LOAD_BASE; g_hi = USER_STACK_TOP; }
static void map_none(void) { g_yes = 0; g_lo = g_hi = 0; }
static void map_yes(void) { g_yes = 1; }   /* everything mapped */

static void test_buffer_range(void) {
    map_all();
    TEST("buffer: zero count is always valid");
    CHECK_EQ(user_buffer_valid((void *)0, 0), 1);            /* even NULL */
    CHECK_EQ(user_buffer_valid((void *)0xDEADBEEF, 0), 1);

    TEST("buffer: must lie in the user range");
    CHECK_EQ(user_buffer_valid((void *)(USER_LOAD_BASE - 1), 4), 0);
    CHECK_EQ(user_buffer_valid((void *)USER_LOAD_BASE, 4), 1);
    CHECK_EQ(user_buffer_valid((void *)(USER_STACK_TOP - 4), 4), 1);
    CHECK_EQ(user_buffer_valid((void *)USER_STACK_TOP, 4), 0);        /* at top */
    CHECK_EQ(user_buffer_valid((void *)(USER_STACK_TOP + 0x1000), 4), 0);
}

static void test_buffer_mmap_range(void) {
    map_yes();
    TEST("buffer: mapped mmap pages are valid user memory");
    CHECK_EQ(user_buffer_valid((void *)USER_EXT_BASE, 4), 1);
    CHECK_EQ(user_buffer_valid((void *)(USER_EXT_TOP - 4), 4), 1);
    CHECK_EQ(user_buffer_valid((void *)(USER_EXT_TOP - 4), 8), 0);
    CHECK_EQ(user_buffer_valid((void *)USER_EXT_TOP, 4), 0);
    CHECK_EQ(user_buffer_valid((void *)(USER_STACK_TOP + 0x1000), 4), 0);

    TEST("buffer: the mapped shared page is valid user memory");
    CHECK_EQ(user_buffer_valid((void *)USER_SHM_BASE, 4), 1);
    CHECK_EQ(user_buffer_valid((void *)(USER_SHM_TOP - 4), 4), 1);
    CHECK_EQ(user_buffer_valid((void *)(USER_SHM_TOP - 4), 8), 0);
}

static void test_buffer_overflow(void) {
    /* Force the mapping oracle to say "mapped" so ONLY the bound arithmetic
     * decides -- otherwise the oracle rejecting a huge range would mask an
     * overflow bug in the bound itself. */
    map_yes();
    TEST("buffer: count must not run past the top");
    /* 4 bytes left, ask for 8 -> rejected. */
    CHECK_EQ(user_buffer_valid((void *)(USER_STACK_TOP - 4), 8), 0);
    /* Exactly reaching the top is fine. */
    CHECK_EQ(user_buffer_valid((void *)(USER_STACK_TOP - 8), 8), 1);

    TEST("buffer: huge count cannot overflow the bound");
    /* start just below the top, count = 4 GB - 1: TOP - start = 1, so
     * a correct `count <= TOP - start` rejects; a naive start+count would
     * wrap to a small value and wrongly accept. */
    CHECK_EQ(user_buffer_valid((void *)(USER_STACK_TOP - 1), 0xFFFFFFFFu), 0);
    CHECK_EQ(user_buffer_valid((void *)USER_LOAD_BASE, 0xFFFFFFFFu), 0);
    /* A count large enough that start+count wraps back below the top: with the
     * buggy `start + count` bound this wrongly passes; the correct bound
     * rejects it. start = TOP-0x10, count chosen so start+count = TOP-0x8. */
    CHECK_EQ(user_buffer_valid((void *)(USER_STACK_TOP - 0x10),
                               0xFFFFFFFFu - 0x8 + 1), 0);
}

static void test_buffer_mapping(void) {
    TEST("buffer: rejected when the range is not mapped");
    map_none();
    CHECK_EQ(user_buffer_valid((void *)USER_LOAD_BASE, 16), 0);

    /* Mapped only up to a point: a range crossing the edge is rejected. */
    g_lo = USER_LOAD_BASE;
    g_hi = USER_LOAD_BASE + 0x1000;
    CHECK_EQ(user_buffer_valid((void *)USER_LOAD_BASE, 0x1000), 1);
    CHECK_EQ(user_buffer_valid((void *)USER_LOAD_BASE, 0x1001), 0);
    CHECK_EQ(user_buffer_valid((void *)(USER_LOAD_BASE + 0x800), 0x800), 1);
    CHECK_EQ(user_buffer_valid((void *)(USER_LOAD_BASE + 0x800), 0x801), 0);
}

static void test_alloc_fd(void) {
    open_file_t files[MAX_OPEN_FILES];
    int i;

    TEST("alloc_fd hands out distinct descriptors then fails");
    for (i = 0; i < MAX_OPEN_FILES; i++) files[i].kind = OF_NONE;

    for (i = 0; i < MAX_OPEN_FILES; i++) {
        int32_t fd = alloc_fd(files, OF_FILE, NULL, NULL);
        CHECK_EQ(fd, FIRST_USER_FD + i);          /* sequential from 3 */
    }
    CHECK_EQ(alloc_fd(files, OF_FILE, NULL, NULL), -1);   /* table full */

    /* Freeing a middle slot lets it be reused at that index. */
    files[2].kind = OF_NONE;
    CHECK_EQ(alloc_fd(files, OF_FILE, NULL, NULL), FIRST_USER_FD + 2);

    CHECK_EQ(alloc_fd(NULL, OF_FILE, NULL, NULL), -1);    /* NULL table */
}

/* --- user_string_valid: needs real memory at the user addresses ----------- */
static volatile char *g_user;     /* mapped view of [USER_LOAD_BASE, TOP) */
static volatile char *g_ext;      /* mapped view of [USER_EXT_BASE, EXT_TOP) */

static void test_string(void) {
    uint32_t region = USER_STACK_TOP - USER_LOAD_BASE;
    char *p;
    uint32_t i;

    if (!g_user) {
        TEST("string tests skipped (could not map the user region)");
        CHECK(1);
        return;
    }
    map_all();
    p = (char *)g_user;

    TEST("string: below/above the user range");
    CHECK_EQ(user_string_valid((char *)(USER_LOAD_BASE - 1)), 0);
    CHECK_EQ(user_string_valid((char *)USER_STACK_TOP), 0);

    TEST("string: a normal NUL-terminated string");
    /* place "hello" at offset 0x1000 */
    for (i = 0; i < 6; i++) p[0x1000 + i] = "hello"[i];
    CHECK_EQ(user_string_valid((char *)(USER_LOAD_BASE + 0x1000)), 1);

    TEST("string: no terminator within MAX_USER_STRING");
    for (i = 0; i < MAX_USER_STRING + 8; i++) p[0x2000 + i] = 'A';   /* no NUL */
    CHECK_EQ(user_string_valid((char *)(USER_LOAD_BASE + 0x2000)), 0);
    /* a NUL exactly at the last scanned byte is accepted */
    p[0x2000 + (MAX_USER_STRING - 1)] = '\0';
    CHECK_EQ(user_string_valid((char *)(USER_LOAD_BASE + 0x2000)), 1);

    TEST("string: unmapped page is rejected before deref runs off");
    map_none();
    CHECK_EQ(user_string_valid((char *)(USER_LOAD_BASE + 0x1000)), 0);
    map_all();

    TEST("string: scan is clamped near the top of the range");
    /* 10 bytes below the top, filled without a NUL -> clamped scan finds none */
    {
        uint32_t off = region - 10;
        for (i = 0; i < 10; i++) p[off + i] = 'B';
        CHECK_EQ(user_string_valid((char *)(USER_LOAD_BASE + off)), 0);
        p[off + 5] = '\0';
        CHECK_EQ(user_string_valid((char *)(USER_LOAD_BASE + off)), 1);
    }
}

static void test_string_mmap_range(void) {
    uint32_t i;

    if (!g_ext) {
        TEST("mmap string tests skipped (could not map the extended region)");
        CHECK(1);
        return;
    }

    g_lo = USER_EXT_BASE;
    g_hi = USER_EXT_TOP;
    for (i = 0; i < 6; i++) g_ext[0x1000 + i] = "hello"[i];

    TEST("string: mapped mmap memory is a valid source");
    CHECK_EQ(user_string_valid((char *)(USER_EXT_BASE + 0x1000)), 1);
    CHECK_EQ(user_string_valid((char *)USER_EXT_TOP), 0);
}

int main(void) {
    /* Try to place real memory at the fixed user addresses for the string
     * tests; the buffer/alloc tests do not need it. */
    void *want = (void *)(uintptr_t)USER_LOAD_BASE;
    size_t len = USER_STACK_TOP - USER_LOAD_BASE;
    void *got = mmap(want, len, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    void *ext_want = (void *)(uintptr_t)USER_EXT_BASE;
    size_t ext_len = USER_EXT_TOP - USER_EXT_BASE;
    void *ext_got = mmap(ext_want, ext_len, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                         -1, 0);
    g_user = (got == want) ? (volatile char *)got : NULL;
    g_ext = (ext_got == ext_want) ? (volatile char *)ext_got : NULL;

    test_buffer_range();
    test_buffer_mmap_range();
    test_buffer_overflow();
    test_buffer_mapping();
    test_alloc_fd();
    test_string();
    test_string_mmap_range();
    TEST_REPORT("syscall-valid");
}
