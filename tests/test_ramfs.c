#include "../ramfs.h"

#include "test.h"
#include "fs_conformance.h"

#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

/*
 * RAMFS is the root filesystem: every embedded program lives on it, and its
 * open-reference counting is what makes the F11 and F20 fixes correct -- both
 * rely on "a file that is open cannot be unlinked". That property has only ever
 * been tested indirectly, through the shell.
 *
 * Two things about this module make it worth testing directly:
 *
 *   - fs_node_t::impl carries the RAMFS_DYNAMIC flag in bit 0 AND the open
 *     reference count in the bits above it. Every open/close has to leave the
 *     flag alone, or a file becomes either permanently undeletable or deletable
 *     while still in use -- and the second one is a kernel use-after-free.
 *
 *   - ramfs_write grows the buffer geometrically (PERF2). `make bench` measures
 *     that it is fast; nothing checked that it is correct at the boundaries,
 *     including the capacity arithmetic near 2^32.
 *
 * A watchdog is armed for the whole run: the bug this file was written to pin
 * down (F22) is an INFINITE LOOP in that capacity arithmetic, so a regression
 * must fail the suite rather than hang it.
 */

/* --- watchdog ------------------------------------------------------------- */
static void on_alarm(int sig) {
    /* Only async-signal-safe calls here. */
    static const char msg[] =
        "  FAIL watchdog fired: ramfs_write did not terminate (F22 regression?)\n";
    ssize_t ignored = write(2, msg, sizeof(msg) - 1);

    (void)sig;
    (void)ignored;
    _exit(1);
}

/* --- allocator stub ------------------------------------------------------- */
/* Counting kmalloc so the test can tell a reallocation from a capacity reuse,
 * and refuse absurd sizes deterministically instead of depending on how the
 * host's malloc happens to answer a 2GB request. */
static int      g_allocs;
static int      g_frees;
static uint32_t g_alloc_limit = 1u << 20;   /* refuse anything larger */

void *kmalloc(size_t size) {
    void *p;

    if (size > g_alloc_limit) return NULL;
    g_allocs++;
    p = malloc(size ? size : 1);
    /* Poison every fresh block. The real kmalloc hands back recycled heap
     * memory that still holds whatever the previous owner left there, so
     * zeroing is the filesystem's job. Returning clean memory here would let a
     * missing zero-fill pass: the hole in a sparse file would read back as 0
     * because malloc happened to be generous, not because ramfs did its work.
     * With poison, any byte the file exposes without writing it shows up. */
    if (p) {
        unsigned char *b = (unsigned char *)p;
        size_t n = size ? size : 1;
        while (n--) *b++ = 0xAA;
    }
    return p;
}

void kfree(void *p) {
    if (p) g_frees++;
    free(p);
}

/* ramfs.c only needs the root pointer from the VFS layer, so fs.c is not
 * linked -- that keeps the stub surface to the allocator alone. */
fs_node_t *fs_root = NULL;

/* --- helpers -------------------------------------------------------------- */
static int bytes_equal(const void *a, const void *b, uint32_t n) {
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (uint32_t i = 0; i < n; i++) if (x[i] != y[i]) return 0;
    return 1;
}

static int all_zero(const void *p, uint32_t n) {
    const uint8_t *b = (const uint8_t *)p;
    for (uint32_t i = 0; i < n; i++) if (b[i]) return 0;
    return 1;
}

static uint32_t write_at(fs_node_t *n, uint32_t off, const char *s, uint32_t len) {
    return n->write(n, off, len, (uint8_t *)s);
}

/* Wipe the whole filesystem between tests: node_count is file-static, so the
 * only way back to a clean slate is to unlink everything reachable. Simpler and
 * more robust: each test uses distinct names. This just re-inits the root. */
static void fs_reset(void) {
    g_allocs = g_frees = 0;
    g_alloc_limit = 1u << 20;
    if (!fs_root) ramfs_init();
}

/* --- tests ---------------------------------------------------------------- */

static void test_init_and_lookup(void) {
    fs_reset();
    TEST("init creates a root directory");
    CHECK(fs_root != NULL);
    CHECK_EQ(fs_root->flags, FS_DIRECTORY);
    /* The root must not be RAMFS_DYNAMIC, or it could be removed. */
    CHECK_EQ(fs_root->impl, 0);

    TEST("root resolves through several spellings");
    CHECK(ramfs_find_node("/") == fs_root);
    CHECK(ramfs_find_node("") == fs_root);
}

static void test_create_read_write(void) {
    fs_node_t *f;

    fs_reset();
    TEST("a created file is readable and writable");
    f = ramfs_create_file("/a.txt");
    CHECK(f != NULL);
    CHECK_EQ(f->flags, FS_FILE);
    CHECK_EQ(f->length, 0);
    CHECK_EQ(f->impl, 1);          /* RAMFS_DYNAMIC, no open references */
    CHECK(f->read && f->write && f->open && f->close);

    CHECK_EQ(write_at(f, 0, "hello", 5), 5);
    CHECK_EQ(f->length, 5);
    {
        char buf[8] = {0};
        CHECK_EQ(f->read(f, 0, 5, (uint8_t *)buf), 5);
        CHECK(bytes_equal(buf, "hello", 5));
    }

    TEST("reads are clamped to the file, not the request");
    {
        char buf[16] = {0};
        CHECK_EQ(f->read(f, 0, 16, (uint8_t *)buf), 5);   /* short read */
        CHECK_EQ(f->read(f, 5, 1, (uint8_t *)buf), 0);    /* at EOF */
        CHECK_EQ(f->read(f, 99, 1, (uint8_t *)buf), 0);   /* past EOF */
    }

    TEST("an in-place overwrite does not change the length");
    CHECK_EQ(write_at(f, 0, "HELLO", 5), 5);
    CHECK_EQ(f->length, 5);
    {
        char buf[8] = {0};
        f->read(f, 0, 5, (uint8_t *)buf);
        CHECK(bytes_equal(buf, "HELLO", 5));
    }

    TEST("a duplicate name is refused");
    CHECK(ramfs_create_file("/a.txt") == NULL);
}

static void test_sparse_write_zero_fills(void) {
    fs_node_t *f;
    char buf[64];

    fs_reset();
    TEST("a write past the end zero-fills the gap");
    /* The gap must never expose whatever the allocator handed back, which on a
     * reused heap block is another file's data. */
    f = ramfs_create_file("/sparse");
    CHECK(f != NULL);
    CHECK_EQ(write_at(f, 0, "AB", 2), 2);
    CHECK_EQ(write_at(f, 40, "Z", 1), 1);
    CHECK_EQ(f->length, 41);

    for (int i = 0; i < 64; i++) buf[i] = (char)0xEE;   /* poison the reader */
    CHECK_EQ(f->read(f, 0, 41, (uint8_t *)buf), 41);
    CHECK(bytes_equal(buf, "AB", 2));
    CHECK(all_zero(buf + 2, 38));       /* the hole */
    CHECK_EQ(buf[40], 'Z');
}

static void test_sparse_write_that_also_grows(void) {
    static char big[1200];
    fs_node_t *f;

    fs_reset();
    TEST("a sparse write that also reallocates zero-fills the gap");
    /* ramfs_write has two zero-fills -- one on the reallocation path, one on the
     * capacity-reuse path -- and they cover for each other, so removing either
     * alone is invisible if the hole always lands inside the existing capacity
     * (as it does in the test above: 41 < the initial 64).
     *
     * Pushing the hole PAST the capacity forces the reallocation branch and
     * leaves the reuse branch unexecuted, which isolates the growth fill. That
     * fill is the one that establishes the "[length, capacity) is zero"
     * invariant the other branch relies on. */
    f = ramfs_create_file("/sparse2");
    CHECK(f != NULL);
    CHECK_EQ(write_at(f, 0, "AB", 2), 2);
    CHECK_EQ(write_at(f, 1000, "Z", 1), 1);   /* 1001 > 64: must reallocate */
    CHECK_EQ(f->length, 1001);

    for (int i = 0; i < 1200; i++) big[i] = (char)0xEE;
    CHECK_EQ(f->read(f, 0, 1001, (uint8_t *)big), 1001);
    CHECK(bytes_equal(big, "AB", 2));
    CHECK(all_zero(big + 2, 998));            /* the hole, across the realloc */
    CHECK_EQ(big[1000], 'Z');

    TEST("spare capacity past the length is not exposed by a later append");
    /* The invariant again, from the other side: appending into the spare room
     * must show zeros, not whatever the allocator had in there. */
    CHECK_EQ(write_at(f, 1100, "!", 1), 1);
    CHECK_EQ(f->length, 1101);
    for (int i = 0; i < 1200; i++) big[i] = (char)0xEE;
    CHECK_EQ(f->read(f, 1001, 100, (uint8_t *)big), 100);
    CHECK(all_zero(big, 99));
    CHECK_EQ(big[99], '!');
}

static void test_geometric_growth(void) {
    fs_node_t *f;
    int allocs_after_first;

    fs_reset();
    TEST("repeated appends reuse spare capacity");
    /* PERF2: growth is geometric, so appending byte by byte must not reallocate
     * every time. 64 is the first capacity, so the first 64 bytes take exactly
     * one allocation. */
    f = ramfs_create_file("/grow");
    CHECK(f != NULL);
    CHECK_EQ(write_at(f, 0, "x", 1), 1);
    allocs_after_first = g_allocs;
    for (uint32_t i = 1; i < 64; i++) CHECK_EQ(write_at(f, i, "x", 1), 1);
    CHECK_EQ(f->length, 64);
    CHECK_EQ(g_allocs, allocs_after_first);   /* no further reallocation */

    TEST("crossing the capacity reallocates once and keeps the contents");
    CHECK_EQ(write_at(f, 64, "y", 1), 1);
    CHECK_EQ(g_allocs, allocs_after_first + 1);
    CHECK_EQ(f->length, 65);
    {
        char buf[80];
        CHECK_EQ(f->read(f, 0, 65, (uint8_t *)buf), 65);
        for (int i = 0; i < 64; i++) CHECK(buf[i] == 'x');
        CHECK(buf[64] == 'y');
    }

    TEST("growth is logarithmic in the number of appends");
    /* 64 -> 128 -> ... : filling to 4096 from a 64-byte start is 6 doublings,
     * so a handful of allocations rather than one per write. */
    {
        int before = g_allocs;
        for (uint32_t i = 65; i < 4096; i++) CHECK_EQ(write_at(f, i, "z", 1), 1);
        CHECK_EQ(f->length, 4096);
        CHECK(g_allocs - before <= 8);
    }
}

static void test_huge_offset_terminates(void) {
    fs_node_t *f;

    fs_reset();
    TEST("F22: a write near 2^31 terminates instead of spinning forever");
    /* The capacity loop doubles 64, 128, ... until it covers new_length. At
     * 0x80000000 the next doubling wraps to 0, and 0 neither reaches new_length
     * nor trips a `> 0x80000000` guard, so the loop never exits. sys_seek allows
     * an offset up to 0x7FFFFFFF, so a user program can reach this with a
     * two-byte write -- and int 0x80 is an interrupt gate, so the spin happens
     * with interrupts disabled: the entire machine stops.
     *
     * The allocation cannot succeed either way; what matters is that the call
     * RETURNS. The watchdog turns a regression into a failure, not a hang. */
    f = ramfs_create_file("/huge");
    CHECK(f != NULL);
    CHECK_EQ(write_at(f, 0x7FFFFFFFU, "ab", 2), 0);   /* refused, cleanly */
    CHECK_EQ(f->length, 0);                           /* and nothing recorded */

    TEST("F22: the exact wrap boundary terminates too");
    /* new_length == 0x80000001 is the first size that needs a capacity past the
     * doubling limit. */
    CHECK_EQ(write_at(f, 0x80000000U, "a", 1), 0);
    CHECK_EQ(write_at(f, 0xFFFFFFFEU, "a", 1), 0);
    CHECK_EQ(f->length, 0);

    TEST("an offset+size overflow is refused");
    CHECK_EQ(write_at(f, 0xFFFFFFFFU, "ab", 2), 0);
    CHECK_EQ(f->length, 0);

    TEST("a zero-length write is a no-op");
    CHECK_EQ(write_at(f, 0, "", 0), 0);
    CHECK_EQ(f->length, 0);
}

static void test_open_reference_blocks_unlink(void) {
    fs_node_t *f;

    fs_reset();
    TEST("an open file cannot be unlinked");
    /* This is the property F11 and F20 depend on. If it breaks, the kernel
     * frees a node that a process (or the ELF loader) is still reading through,
     * and the next access calls a function pointer out of freed heap memory. */
    f = ramfs_create_file("/pinned");
    CHECK(f != NULL);
    f->open(f);
    CHECK_EQ(ramfs_unlink_file("/pinned"), -1);
    CHECK(ramfs_find_node("/pinned") == f);      /* still there */

    TEST("closing the last reference allows the unlink");
    f->close(f);
    CHECK_EQ(ramfs_unlink_file("/pinned"), 0);
    CHECK(ramfs_find_node("/pinned") == NULL);

    TEST("references nest: every open needs its own close");
    f = ramfs_create_file("/nested");
    CHECK(f != NULL);
    f->open(f);
    f->open(f);
    f->open(f);
    CHECK_EQ(ramfs_unlink_file("/nested"), -1);
    f->close(f);
    CHECK_EQ(ramfs_unlink_file("/nested"), -1);   /* two left */
    f->close(f);
    CHECK_EQ(ramfs_unlink_file("/nested"), -1);   /* one left */
    f->close(f);
    CHECK_EQ(ramfs_unlink_file("/nested"), 0);    /* now unpinned */
}

static void test_impl_flag_survives_references(void) {
    fs_node_t *f;

    fs_reset();
    TEST("open/close leave the RAMFS_DYNAMIC flag intact");
    /* The flag and the count share one word. If open/close disturbed bit 0 the
     * file would become either undeletable forever or -- far worse -- deletable
     * while still open. */
    f = ramfs_create_file("/flag");
    CHECK(f != NULL);
    CHECK_EQ(f->impl & 1u, 1);
    f->open(f);
    CHECK_EQ(f->impl & 1u, 1);
    CHECK(f->impl > 1);              /* a reference is recorded above the flag */
    f->close(f);
    CHECK_EQ(f->impl, 1);            /* exactly back to "dynamic, unreferenced" */

    TEST("an unmatched close cannot forge a reference count");
    /* Closing more times than opened must not underflow into the flag bit and
     * must not leave a phantom reference behind. */
    f->close(f);
    f->close(f);
    CHECK_EQ(f->impl, 1);
    CHECK_EQ(ramfs_unlink_file("/flag"), 0);   /* still deletable */
}

static void test_static_files_are_immutable(void) {
    static const uint8_t data[] = "embedded";
    fs_node_t *f;

    fs_reset();
    TEST("a static file is read-only and never removable");
    /* This is what protects the embedded programs: they point into kernel
     * .rodata, so kfree() on them would corrupt the heap. */
    f = ramfs_create_static_file("/static", data, sizeof(data) - 1);
    CHECK(f != NULL);
    CHECK_EQ(f->impl, 0);            /* not RAMFS_DYNAMIC */
    CHECK(f->write == NULL);
    CHECK_EQ(f->length, sizeof(data) - 1);
    CHECK_EQ(ramfs_unlink_file("/static"), -1);
    CHECK(ramfs_find_node("/static") == f);

    TEST("an open/close cycle does not make a static file removable");
    /* impl starts at 0 here, so a close that underflowed would wrap to
     * 0xFFFFFFFE and the DYNAMIC test would still reject it -- but an open
     * followed by two closes must not land on 1 (= dynamic, unreferenced). */
    f->open(f);
    f->close(f);
    f->close(f);
    CHECK_EQ(f->impl, 0);
    CHECK_EQ(ramfs_unlink_file("/static"), -1);
}

static void test_directories(void) {
    fs_node_t *d;

    fs_reset();
    TEST("directories nest and resolve");
    CHECK_EQ(ramfs_create_directory("/dir"), 0);
    d = ramfs_find_node("/dir");
    CHECK(d != NULL);
    CHECK_EQ(d->flags, FS_DIRECTORY);
    CHECK_EQ(ramfs_create_directory("/dir/sub"), 0);
    CHECK(ramfs_find_node("/dir/sub") != NULL);
    CHECK(ramfs_create_file("/dir/sub/deep") != NULL);
    CHECK(ramfs_find_node("/dir/sub/deep") != NULL);

    TEST("a non-empty directory cannot be removed");
    CHECK_EQ(ramfs_remove_directory("/dir"), -1);
    CHECK_EQ(ramfs_remove_directory("/dir/sub"), -1);

    TEST("removal succeeds bottom-up");
    CHECK_EQ(ramfs_unlink_file("/dir/sub/deep"), 0);
    CHECK_EQ(ramfs_remove_directory("/dir/sub"), 0);
    CHECK_EQ(ramfs_remove_directory("/dir"), 0);
    CHECK(ramfs_find_node("/dir") == NULL);

    TEST("the root cannot be removed");
    CHECK_EQ(ramfs_remove_directory("/"), -1);
    CHECK(fs_root != NULL);

    TEST("a directory is not a file and vice versa");
    CHECK_EQ(ramfs_create_directory("/d2"), 0);
    CHECK(ramfs_find_file("/d2") == NULL);      /* find_file filters directories */
    CHECK_EQ(ramfs_unlink_file("/d2"), -1);     /* unlink is files only */
    CHECK_EQ(ramfs_remove_directory("/d2"), 0);
}

static void test_path_parsing(void) {
    fs_reset();
    TEST("malformed paths are refused, not misresolved");
    CHECK(ramfs_create_file("/p.txt") != NULL);
    CHECK(ramfs_find_node("/p.txt") != NULL);

    CHECK(ramfs_find_node("//p.txt") == NULL);     /* empty component */
    CHECK(ramfs_find_node("/p.txt/") == NULL);     /* trailing slash */
    CHECK(ramfs_find_node("/./p.txt") == NULL);    /* "." is not a component */
    CHECK(ramfs_find_node("/../p.txt") == NULL);   /* nor ".." */
    CHECK(ramfs_find_node("/nope") == NULL);
    CHECK(ramfs_find_node(NULL) == NULL);

    TEST("an over-long path is refused rather than truncated");
    /* Truncation is what F8 fixed in the VFS layer; the same class of bug here
     * would resolve to an ancestor and act on the wrong object. */
    {
        char path[256];
        int i = 0;
        path[i++] = '/';
        while (i < 200) path[i++] = 'a';
        path[i] = '\0';
        CHECK(ramfs_find_node(path) == NULL);
        CHECK(ramfs_create_file(path) == NULL);
    }

    TEST("an over-long single component is refused");
    {
        char path[160];
        int i = 0;
        path[i++] = '/';
        while (i < 140) path[i++] = 'b';
        path[i] = '\0';
        CHECK(ramfs_create_file(path) == NULL);
    }

    TEST("an empty name is refused");
    CHECK(ramfs_create_file("/") == NULL);
    CHECK(ramfs_create_file("") == NULL);
}

static void test_out_of_memory_is_clean(void) {
    fs_node_t *f;

    fs_reset();
    TEST("a failed growth leaves the file untouched");
    /* The fallback path (geometric size refused, exact size tried) must not
     * half-apply: a length recorded without a buffer to back it would make the
     * next read walk off the allocation. */
    f = ramfs_create_file("/oom");
    CHECK(f != NULL);
    CHECK_EQ(write_at(f, 0, "keep", 4), 4);

    g_alloc_limit = 8;                       /* nothing sizeable can be had */
    /* The write has to cross the CAPACITY, not just the length: the first write
     * already claimed 64 bytes, so appending inside that spare room needs no
     * allocation at all and would succeed (which is the point of PERF2). */
    CHECK_EQ(write_at(f, 64, "more", 4), 0); /* refused */
    CHECK_EQ(f->length, 4);                  /* unchanged */
    {
        char buf[8] = {0};
        CHECK_EQ(f->read(f, 0, 4, (uint8_t *)buf), 4);
        CHECK(bytes_equal(buf, "keep", 4));  /* and still readable */
    }

    TEST("node creation fails cleanly when the allocator refuses");
    g_alloc_limit = 0;
    CHECK(ramfs_create_file("/nomem") == NULL);
    CHECK(ramfs_find_node("/nomem") == NULL);
}

static void test_node_table_limit(void) {
    char name[32];
    int created = 0;
    uint32_t count_at_limit;

    fs_reset();
    TEST("the node table is bounded, and refusal is graceful");
    /* MAX_RAMFS_NODES is private; create until it refuses and check the
     * filesystem is still consistent afterwards rather than corrupted.
     *
     * The absolute node count is NOT asserted: node_count is file-static and
     * the earlier tests deliberately leave files behind, so the table starts
     * partly full. What matters is that it refuses and then stays put. */
    for (int i = 0; i < 200; i++) {
        int n = 0;
        name[n++] = '/';
        name[n++] = 'n';
        name[n++] = (char)('0' + (i / 100) % 10);
        name[n++] = (char)('0' + (i / 10) % 10);
        name[n++] = (char)('0' + i % 10);
        name[n] = '\0';
        if (!ramfs_create_file(name)) break;
        created++;
    }
    CHECK(created > 0);
    CHECK(created < 200);                    /* it did refuse at some point */

    TEST("a full table keeps refusing and stops counting up");
    count_at_limit = ramfs_get_node_count();
    CHECK(ramfs_create_file("/extra") == NULL);
    CHECK(ramfs_create_directory("/extradir") == -1);
    CHECK_EQ(ramfs_get_node_count(), count_at_limit);   /* no silent overrun */

    TEST("the filesystem is still consistent after the table filled up");
    CHECK(ramfs_find_node("/n000") != NULL);
    CHECK(ramfs_find_node("/") == fs_root);
    CHECK(ramfs_find_node("/extra") == NULL);

    TEST("freeing a slot lets creation work again");
    CHECK_EQ(ramfs_unlink_file("/n000"), 0);
    CHECK_EQ(ramfs_get_node_count(), count_at_limit - 1);
    CHECK(ramfs_create_file("/reused") != NULL);
    CHECK(ramfs_find_node("/reused") != NULL);
    /* And the surviving neighbours were not disturbed by the table compaction
     * that removal performs. */
    CHECK(ramfs_find_node("/n001") != NULL);
    CHECK(ramfs_find_node("/") == fs_root);
}


/* The shared contract every backend owes (see tests/fs_conformance.h). RAMFS
 * is where F22 came from, so this is the backend the contract was written
 * against; the allocator stub refuses anything over a megabyte, which is what
 * makes "the allocation cannot succeed" deterministic rather than dependent on
 * how the host's malloc feels about a two-gigabyte request. */
static void test_backend_conformance(void) {
    static const uint8_t content[] = { 'r', 'a', 'm', 'f', 's' };
    fs_node_t *f;

    /* Earlier tests leave the allocator stub refusing allocations on
     * purpose. Restore the default ceiling so this test depends on the
     * contract rather than on where it happens to sit in main(): it still
     * has to be low enough that a two-gigabyte request cannot succeed,
     * which is what makes "the backend cannot reach that offset"
     * deterministic instead of a question about the host's malloc. */
    g_alloc_limit = 1u << 20;
    f = ramfs_create_file("/conf.txt");

    CHECK(f != NULL);
    if (!f) return;
    CHECK_EQ(f->write(f, 0, sizeof(content), (uint8_t *)content),
             sizeof(content));

    fs_conformance_extreme_offsets(f, content, sizeof(content), "ramfs");
}

int main(void) {
    /* A hang here would stall `make test`; fail loudly instead. */
    signal(SIGALRM, on_alarm);
    alarm(30);

    test_init_and_lookup();
    test_create_read_write();
    test_sparse_write_zero_fills();
    test_sparse_write_that_also_grows();
    test_geometric_growth();
    test_huge_offset_terminates();
    test_open_reference_blocks_unlink();
    test_impl_flag_survives_references();
    test_static_files_are_immutable();
    test_directories();
    test_path_parsing();
    test_out_of_memory_is_clean();
    /* Before test_node_table_limit(): that one deliberately fills the
     * node table to MAX_RAMFS_NODES, after which nothing can create the
     * file the contract needs. */
    test_backend_conformance();
    test_node_table_limit();

    alarm(0);
    TEST_REPORT("ramfs");
}
