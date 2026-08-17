#include "test.h"
#include "fs_conformance.h"
#include "../fat16.h"
#include "../fs.h"
#include "../pmm.h"

#include <stdlib.h>

/*
 * FAT16 keeps a whole filesystem in a byte array, which makes it the module
 * that benefits most from running natively: the cluster-chain walk, the 8.3
 * name encoding and the allocate-on-write path are all reachable directly,
 * whereas the shell-driven suite only ever touches three small files that fit
 * in a single cluster each.
 *
 * Two things here are impossible to test through QEMU at all. Filling the
 * volume until allocation fails would wreck the shared image for every later
 * assertion in that boot; here each test re-mounts a pristine copy. And
 * reading back at awkward offsets across cluster boundaries needs far more
 * data than the embedded files contain.
 *
 * fat16_install copies the image through pmm_alloc_blocks, so stub that with
 * host memory -- it is the only pmm entry point fat16 uses.
 */

extern const uint8_t  fat16_image_data[];
extern const uint32_t fat16_image_size;

static uint32_t pmm_outstanding_blocks;
static uint32_t pmm_alloc_calls;
static uint32_t pmm_free_calls;
static int      pmm_bad_free;

void *pmm_alloc_blocks(uint32_t count) {
    void *p = NULL;
    if (count == 0) return NULL;
    if (posix_memalign(&p, PMM_BLOCK_SIZE, (size_t)count * PMM_BLOCK_SIZE) != 0)
        return NULL;
    pmm_outstanding_blocks += count;
    pmm_alloc_calls++;
    return p;
}

void pmm_free_blocks(void *p, uint32_t count) {
    if (!p || count == 0) return;
    if (count > pmm_outstanding_blocks) {
        pmm_bad_free = 1;
    } else {
        pmm_outstanding_blocks -= count;
    }
    pmm_free_calls++;
    free(p);
}

#define HELLO      "Hello from FAT16!\n"
#define HELLO_LEN  18
#define NOTE       "nested fat note\n"
#define NOTE_LEN   16

/* Create through the directory node's own vtable: the VFS create_fs() takes a
 * path and resolves from fs_root, which these tests never populate. */
static fs_node_t *make_file(fs_node_t *dir, const char *name) {
    return (dir && dir->create) ? dir->create(dir, name) : NULL;
}

/* Every test starts from a pristine image so one test cannot skew another. */
static fs_node_t *remount(void) {
    fat16_install(fat16_image_data, fat16_image_size);
    return fat16_get_root_node();
}

static void test_reject_wrapped_layout(void) {
    uint8_t image[512] = { 0 };

    TEST("mount rejects a BPB whose sector offset wraps");

    image[11] = 0x00; image[12] = 0x02; /* 512-byte sectors */
    image[13] = 1;                      /* sectors per cluster */
    image[14] = 127; image[15] = 0;     /* reserved sectors */
    image[16] = 128;                    /* FAT copies */
    image[17] = 16; image[18] = 0;      /* one root-directory sector */
    image[22] = 0xFF; image[23] = 0xFF; /* sectors per FAT */
    image[510] = 0x55; image[511] = 0xAA;

    /* root_dir_lba is 8,388,607 and data_lba is 8,388,608. Multiplying
     * data_lba by 512 in uint32_t wraps to zero even though the supplied
     * image contains only one sector. The image must be rejected rather than
     * exposed as a mounted filesystem with nonsensical offsets. */
    fat16_install(image, sizeof(image));
    CHECK(!fat16_is_mounted());
    CHECK(fat16_get_root_node() == NULL);
    fat16_install(NULL, 0);               /* isolate the following mount tests */
}

static void test_install_ownership(void) {
    uint8_t bad_image[512] = { 0 };
    uint32_t image_blocks = fat16_image_size / PMM_BLOCK_SIZE +
        (fat16_image_size % PMM_BLOCK_SIZE != 0);
    uint32_t alloc_before, free_before;

    TEST("install releases rejected and replaced images");
    CHECK_EQ(pmm_outstanding_blocks, 0);

    /* Even a rejected image was copied before its BPB was validated. The
     * rejected copy must not remain allocated. */
    alloc_before = pmm_alloc_calls;
    free_before = pmm_free_calls;
    fat16_install(bad_image, sizeof(bad_image));
    CHECK(!fat16_is_mounted());
    CHECK(fat16_get_root_node() == NULL);
    CHECK_EQ(pmm_alloc_calls, alloc_before + 1);
    CHECK_EQ(pmm_free_calls, free_before + 1);
    CHECK_EQ(pmm_outstanding_blocks, 0);

    /* Reinstalling replaces the writable image rather than retaining every
     * prior copy. This also exercises the exact block count passed to PMM. */
    CHECK(remount() != NULL);
    CHECK_EQ(pmm_outstanding_blocks, image_blocks);
    free_before = pmm_free_calls;
    CHECK(remount() != NULL);
    CHECK_EQ(pmm_free_calls, free_before + 1);
    CHECK_EQ(pmm_outstanding_blocks, image_blocks);

    /* A failed replacement releases both the old valid mount and the newly
     * rejected copy, leaving the driver unmounted with no retained storage. */
    free_before = pmm_free_calls;
    fat16_install(bad_image, sizeof(bad_image));
    CHECK(!fat16_is_mounted());
    CHECK_EQ(pmm_free_calls, free_before + 2);
    CHECK_EQ(pmm_outstanding_blocks, 0);
    CHECK_EQ(pmm_bad_free, 0);
}

static void test_mount(void) {
    fs_node_t *root;

    TEST("mount");
    root = remount();
    CHECK(fat16_is_mounted());
    CHECK(root != NULL);
    CHECK_EQ(root->flags, FS_DIRECTORY);
    CHECK(root->readdir != NULL);
    CHECK(root->finddir != NULL);
}

static void test_readdir_root(void) {
    fs_node_t *root = remount();
    dirent_t *d;

    /* The generator writes HELLO.TXT, README.TXT and the DOCS directory; the
     * driver must lowercase 8.3 names and stop cleanly past the last entry. */
    TEST("readdir root");
    d = readdir_fs(root, 0);
    CHECK(d != NULL && __builtin_strcmp(d->name, "hello.txt") == 0);
    d = readdir_fs(root, 1);
    CHECK(d != NULL && __builtin_strcmp(d->name, "readme.txt") == 0);
    d = readdir_fs(root, 2);
    CHECK(d != NULL && __builtin_strcmp(d->name, "docs") == 0);
    CHECK(readdir_fs(root, 3) == NULL);
    CHECK(readdir_fs(root, 99) == NULL);
}

static void test_read_file(void) {
    fs_node_t *root = remount();
    fs_node_t *f;
    char buf[64];

    TEST("read file");
    f = finddir_fs(root, "hello.txt");
    CHECK(f != NULL);
    CHECK_EQ(f->flags, FS_FILE);
    CHECK_EQ(f->length, HELLO_LEN);

    CHECK_EQ(read_fs(f, 0, HELLO_LEN, (uint8_t *)buf), HELLO_LEN);
    buf[HELLO_LEN] = '\0';
    CHECK_STREQ(buf, HELLO);

    /* Partial and out-of-range reads must clamp rather than over-read. */
    CHECK_EQ(read_fs(f, 0, 5, (uint8_t *)buf), 5);
    CHECK_EQ(read_fs(f, 6, 100, (uint8_t *)buf), HELLO_LEN - 6);
    CHECK_EQ(read_fs(f, HELLO_LEN, 10, (uint8_t *)buf), 0);
    CHECK_EQ(read_fs(f, HELLO_LEN + 50, 10, (uint8_t *)buf), 0);

    TEST("lookup is case-insensitive");
    CHECK(finddir_fs(root, "HELLO.TXT") != NULL);
    CHECK(finddir_fs(root, "Hello.Txt") != NULL);
    CHECK(finddir_fs(root, "nosuchfile") == NULL);
}

static void test_nested_directory(void) {
    fs_node_t *root = remount();
    fs_node_t *dir, *f;
    char buf[64];

    TEST("nested directory");
    dir = finddir_fs(root, "docs");
    CHECK(dir != NULL);
    CHECK_EQ(dir->flags, FS_DIRECTORY);

    /* "." and ".." must be filtered out of the listing. */
    {
        dirent_t *d = readdir_fs(dir, 0);
        CHECK(d != NULL && __builtin_strcmp(d->name, "note.txt") == 0);
        CHECK(readdir_fs(dir, 1) == NULL);
    }

    f = finddir_fs(dir, "note.txt");
    CHECK(f != NULL);
    CHECK_EQ(f->length, NOTE_LEN);
    CHECK_EQ(read_fs(f, 0, NOTE_LEN, (uint8_t *)buf), NOTE_LEN);
    buf[NOTE_LEN] = '\0';
    CHECK_STREQ(buf, NOTE);
}

static void test_multi_cluster_roundtrip(void) {
    fs_node_t *root = remount();
    fs_node_t *f;
    static unsigned char big[3000], back[3000];
    unsigned i;

    /* 3000 bytes spans six 512-byte clusters, so this is the first test that
     * actually exercises chain extension and the walk-to-offset logic. */
    TEST("multi-cluster write/read");
    for (i = 0; i < sizeof(big); i++) big[i] = (unsigned char)(i * 31 + 7);

    f = make_file(root, "big.txt");
    CHECK(f != NULL);
    if (!f) return;

    CHECK_EQ(write_fs(f, 0, sizeof(big), big), sizeof(big));
    CHECK_EQ(f->length, sizeof(big));

    CHECK_EQ(read_fs(f, 0, sizeof(back), back), sizeof(big));
    for (i = 0; i < sizeof(big); i++) CHECK_EQ(back[i], big[i]);

    /* Read back at offsets that straddle cluster boundaries, in sizes that do
     * not divide evenly -- where an off-by-one in the chain walk would show. */
    TEST("offset reads across clusters");
    {
        static const unsigned offs[] = { 1, 511, 512, 513, 1023, 1024, 2047 };
        for (unsigned k = 0; k < sizeof(offs) / sizeof(offs[0]); k++) {
            unsigned off = offs[k];
            unsigned want = 300;
            unsigned got = read_fs(f, off, want, back);
            CHECK_EQ(got, want);
            for (i = 0; i < got; i++) CHECK_EQ(back[i], big[off + i]);
        }
    }

    /* A write that starts partway in must not disturb its neighbours. */
    TEST("partial overwrite");
    {
        unsigned char patch[100];
        for (i = 0; i < sizeof(patch); i++) patch[i] = 0xE5;
        CHECK_EQ(write_fs(f, 700, sizeof(patch), patch), sizeof(patch));
        CHECK_EQ(read_fs(f, 0, sizeof(back), back), sizeof(big));
        for (i = 0; i < 700; i++) CHECK_EQ(back[i], big[i]);
        for (i = 0; i < sizeof(patch); i++) CHECK_EQ(back[700 + i], 0xE5);
        for (i = 800; i < sizeof(big); i++) CHECK_EQ(back[i], big[i]);
    }
}

static void test_write_out_of_space(void) {
    fs_node_t *root = remount();
    fs_node_t *f;
    static unsigned char huge[40000], back[40000];
    uint32_t written;
    unsigned i;

    /*
     * The volume holds far less than 40 KB, so allocation must fail partway.
     * The bug this pins down: the directory entry used to record the REQUESTED
     * end as the file length, advertising data the cluster chain could not
     * back. The length has to match what was actually stored.
     *
     * This is exactly the case the QEMU suite cannot run -- filling the shared
     * image would break every later FAT assertion in that boot.
     */
    TEST("write past end of volume");
    for (i = 0; i < sizeof(huge); i++) huge[i] = (unsigned char)(i * 17 + 3);

    f = make_file(root, "fill.txt");
    CHECK(f != NULL);
    if (!f) return;

    written = write_fs(f, 0, sizeof(huge), huge);
    CHECK(written > 0);                    /* some of it fit */
    CHECK(written < sizeof(huge));         /* but not all of it */
    CHECK_EQ(f->length, written);          /* length == what was stored */

    /* Everything the driver claims to hold must read back correctly. */
    CHECK_EQ(read_fs(f, 0, sizeof(back), back), written);
    for (i = 0; i < written; i++) CHECK_EQ(back[i], huge[i]);
}

static void test_write_stores_nothing(void) {
    fs_node_t *root = remount();
    fs_node_t *filler, *f, *again;
    static unsigned char huge[40000];
    unsigned char probe[600];
    uint32_t written;
    unsigned i;

    /*
     * The sibling case of "write past end of volume", and the one it left
     * open: a write that stores ZERO bytes. F9's fix records offset + written
     * as the new end of file, which is the true end only when something was
     * written there. Start a write beyond the last cluster the chain can be
     * extended to and nothing is stored -- yet the file used to grow to the
     * seek offset anyway, claiming a length its own chain could not back.
     *
     * Reachable from user space: the volume is 32 KB, sys_seek accepts any
     * offset up to 0x7FFFFFFF, and both are ordinary shell operations. It is
     * the same seek-past-the-end shape as F22, against a different filesystem.
     */
    TEST("write that stores nothing");
    for (i = 0; i < sizeof(huge); i++) huge[i] = (unsigned char)(i * 17 + 3);

    /* A small file that owns exactly one cluster ... */
    f = make_file(root, "stuck.txt");
    CHECK(f != NULL);
    if (!f) return;
    CHECK_EQ(write_fs(f, 0, 4, huge), 4);
    CHECK_EQ(f->length, 4);

    /* ... and then no free clusters left for it to grow into. */
    filler = make_file(root, "full.txt");
    CHECK(filler != NULL);
    if (!filler) return;
    CHECK(write_fs(filler, 0, sizeof(huge), huge) > 0);

    /* Write starting well past the one cluster the file has. Nothing can be
     * stored, so nothing may change. */
    written = write_fs(f, 4096, 4, huge);
    CHECK_EQ(written, 0);
    CHECK_EQ(f->length, 4);

    /* The persisted directory entry has to agree with the node: a length
     * corrected only in RAM would come back wrong on the next lookup. */
    again = root->finddir(root, "stuck.txt");
    CHECK(again != NULL);
    if (again) CHECK_EQ(again->length, 4);

    /* And the file still reads back as the four bytes it actually holds --
     * not the remainder of its last cluster served up as file data. */
    for (i = 0; i < sizeof(probe); i++) probe[i] = 0xCC;
    CHECK_EQ(read_fs(f, 0, sizeof(probe), probe), 4);
    for (i = 0; i < 4; i++) CHECK_EQ(probe[i], huge[i]);
    CHECK_EQ(probe[4], 0xCC);              /* untouched past the real end */
}

static void test_unlink_rejects_invalid_cluster_chain(void) {
    uint8_t *image = malloc(fat16_image_size);
    uint32_t bps, reserved, fats, sectors_per_fat;
    uint32_t fat_off, root_off, docs_off, bad_cluster;
    fs_node_t *root;

    TEST("unlink rejects an out-of-volume cluster chain");
    CHECK(image != NULL);
    if (!image) return;
    for (uint32_t i = 0; i < fat16_image_size; i++)
        image[i] = fat16_image_data[i];

    bps = (uint32_t)image[11] | ((uint32_t)image[12] << 8);
    reserved = (uint32_t)image[14] | ((uint32_t)image[15] << 8);
    fats = image[16];
    sectors_per_fat = (uint32_t)image[22] | ((uint32_t)image[23] << 8);
    fat_off = reserved * bps;
    root_off = (reserved + fats * sectors_per_fat) * bps;
    docs_off = root_off + 2 * 32;

    /* Point HELLO at a cluster whose FAT-entry offset aliases the first two
     * bytes of the unrelated DOCS directory entry. It is below FAT16_EOC but
     * well beyond this volume's usable cluster range. */
    bad_cluster = (docs_off - fat_off) / 2;
    CHECK(bad_cluster < 0xFFF8u);
    image[root_off + 26] = (uint8_t)bad_cluster;
    image[root_off + 27] = (uint8_t)(bad_cluster >> 8);

    fat16_install(image, fat16_image_size);
    free(image);
    root = fat16_get_root_node();
    CHECK(root != NULL);
    if (!root) return;
    CHECK(finddir_fs(root, "hello.txt") != NULL);
    CHECK(finddir_fs(root, "docs") != NULL);

    CHECK_EQ(root->unlink(root, "hello.txt"), -1);
    CHECK(finddir_fs(root, "hello.txt") != NULL);
    CHECK(finddir_fs(root, "docs") != NULL);

    TEST("unlink rejects a cyclic cluster chain");
    image = malloc(fat16_image_size);
    CHECK(image != NULL);
    if (!image) return;
    for (uint32_t i = 0; i < fat16_image_size; i++)
        image[i] = fat16_image_data[i];

    /* Turn the two single-cluster HELLO/README chains into 2 -> 3 -> 2. */
    image[fat_off + 2 * 2] = 3;
    image[fat_off + 2 * 2 + 1] = 0;
    image[fat_off + 3 * 2] = 2;
    image[fat_off + 3 * 2 + 1] = 0;
    fat16_install(image, fat16_image_size);
    free(image);

    root = fat16_get_root_node();
    CHECK(root != NULL);
    if (!root) return;
    CHECK_EQ(root->unlink(root, "hello.txt"), -1);
    CHECK(finddir_fs(root, "hello.txt") != NULL);

    TEST("unlink rejects a FAT16 bad-cluster marker");
    image = malloc(fat16_image_size);
    CHECK(image != NULL);
    if (!image) return;
    for (uint32_t i = 0; i < fat16_image_size; i++)
        image[i] = fat16_image_data[i];

    image[fat_off + 2 * 2] = 0xF7;
    image[fat_off + 2 * 2 + 1] = 0xFF;
    fat16_install(image, fat16_image_size);
    free(image);

    root = fat16_get_root_node();
    CHECK(root != NULL);
    if (!root) return;
    CHECK_EQ(root->unlink(root, "hello.txt"), -1);
    CHECK(finddir_fs(root, "hello.txt") != NULL);
}

static void test_zero_byte_write_keeps_clusters_free(void) {
    static unsigned char huge[40000];
    uint8_t payload[4] = { 0x5A, 0xA5, 0x3C, 0xC3 };
    uint32_t bpc =
        (uint32_t)fat16_image_data[13] *
        ((uint32_t)fat16_image_data[11] |
         ((uint32_t)fat16_image_data[12] << 8));
    fs_node_t *root, *target, *filler, *reserve, *probe, *again;
    uint32_t filled;

    TEST("zero-byte writes do not retain provisional clusters");

    /* An offset beyond the volume's theoretical capacity must be rejected
     * before the empty file acquires even its first cluster. */
    root = remount();
    target = make_file(root, "target.txt");
    CHECK(target != NULL);
    if (!target) return;
    CHECK_EQ(write_fs(target, 0x7FFFFFFFu, sizeof(payload), payload), 0);
    CHECK_EQ(target->length, 0);
    CHECK_EQ(target->impl, 0);
    again = finddir_fs(root, "target.txt");
    CHECK(again != NULL);
    if (again) CHECK_EQ(again->impl, 0);

    probe = make_file(root, "probe.txt");
    CHECK(probe != NULL);
    if (probe) CHECK_EQ(write_fs(probe, 0, 1, payload), 1);

    /* Now leave exactly two clusters free and target the fourth cluster of a
     * second empty file. The driver may discover the shortage while extending
     * the chain, but a zero-byte result still has to roll that extension back. */
    root = remount();
    target = make_file(root, "target.txt");
    filler = make_file(root, "fill2.txt");
    CHECK(target != NULL);
    CHECK(filler != NULL);
    if (!target || !filler) return;

    filled = write_fs(filler, 0, sizeof(huge), huge);
    CHECK(filled > 2 * bpc);
    CHECK_EQ(root->unlink(root, "fill2.txt"), 0);

    reserve = make_file(root, "hold.txt");
    CHECK(reserve != NULL);
    if (!reserve || filled <= 2 * bpc) return;
    CHECK_EQ(write_fs(reserve, 0, filled - 2 * bpc, huge), filled - 2 * bpc);

    CHECK_EQ(write_fs(target, 3 * bpc, 1, payload), 0);
    CHECK_EQ(target->length, 0);
    CHECK_EQ(target->impl, 0);
    again = finddir_fs(root, "target.txt");
    CHECK(again != NULL);
    if (again) CHECK_EQ(again->impl, 0);

    probe = make_file(root, "probe.txt");
    CHECK(probe != NULL);
    if (probe) CHECK_EQ(write_fs(probe, 0, 1, payload), 1);
}

static void test_create_unlink(void) {
    fs_node_t *root = remount();
    fs_node_t *f;
    unsigned char data[600];
    unsigned i;

    TEST("create/unlink");
    for (i = 0; i < sizeof(data); i++) data[i] = (unsigned char)i;

    f = make_file(root, "tmp.txt");
    CHECK(f != NULL);
    if (!f) return;
    CHECK_EQ(write_fs(f, 0, sizeof(data), data), sizeof(data));
    CHECK(finddir_fs(root, "tmp.txt") != NULL);

    /* Creating the same name twice must fail rather than duplicate it. */
    CHECK(make_file(root, "tmp.txt") == NULL);

    CHECK_EQ(root->unlink(root, "tmp.txt"), 0);
    CHECK(finddir_fs(root, "tmp.txt") == NULL);
    CHECK_EQ(root->unlink(root, "tmp.txt"), -1);   /* already gone */

    /* Its clusters must return to the pool: the same payload fits again. */
    TEST("unlink frees clusters");
    f = make_file(root, "again.txt");
    CHECK(f != NULL);
    if (f) CHECK_EQ(write_fs(f, 0, sizeof(data), data), sizeof(data));
}

static void test_open_blocks_unlink(void) {
    fs_node_t *root = remount();
    fs_node_t *f;

    /* An open descriptor pins the file: freeing its chain underneath would let
     * those clusters be handed to another file while it is still readable. */
    TEST("open blocks unlink");
    f = finddir_fs(root, "hello.txt");
    CHECK(f != NULL);
    if (!f) return;

    open_fs(f);
    CHECK_EQ(root->unlink(root, "hello.txt"), -1);   /* refused while open */
    close_fs(f);
    CHECK_EQ(root->unlink(root, "hello.txt"), 0);    /* allowed once closed */
}

static void test_name_encoding(void) {
    fs_node_t *root = remount();

    /* 8.3 only: a longer stem or extension has no representation. */
    TEST("8.3 name limits");
    CHECK(make_file(root, "toolongname.txt") == NULL);
    CHECK(make_file(root, "name.toolong") == NULL);
    CHECK(make_file(root, "") == NULL);

    CHECK(make_file(root, "ok.txt") != NULL);
    CHECK(make_file(root, "noext") != NULL);
    CHECK(finddir_fs(root, "ok.txt") != NULL);
    CHECK(finddir_fs(root, "noext") != NULL);
}


/* The shared contract every backend owes (see tests/fs_conformance.h).
 * Deliberately last: reaching for an offset it cannot satisfy makes FAT16
 * allocate every free cluster on the way to giving up, so the volume is full
 * afterwards. Each test remounts a pristine image, so that costs nothing here
 * -- but it would quietly starve any test that ran after it. */
static void test_backend_conformance(void) {
    static const uint8_t content[] = { 'f', 'a', 't' };
    fs_node_t *root = remount();
    fs_node_t *f = make_file(root, "conf.txt");

    CHECK(f != NULL);
    if (!f) return;
    CHECK_EQ(f->write(f, 0, sizeof(content), (uint8_t *)content),
             sizeof(content));

    fs_conformance_extreme_offsets(f, content, sizeof(content), "fat16");
}

int main(void) {
    fs_conformance_arm_watchdog(30);
    test_reject_wrapped_layout();
    test_install_ownership();
    test_mount();
    test_readdir_root();
    test_read_file();
    test_nested_directory();
    test_multi_cluster_roundtrip();
    test_write_out_of_space();
    test_write_stores_nothing();
    test_unlink_rejects_invalid_cluster_chain();
    test_zero_byte_write_keeps_clusters_free();
    test_create_unlink();
    test_open_blocks_unlink();
    test_name_encoding();
    test_backend_conformance();

    /* The production caller mounts once for the lifetime of the kernel. The
     * hosted test remounts repeatedly, so release its final copy as well. */
    TEST("install releases final hosted-test mount");
    fat16_install(NULL, 0);
    CHECK_EQ(pmm_outstanding_blocks, 0);
    CHECK_EQ(pmm_bad_free, 0);
    TEST_REPORT("fat16");
}
