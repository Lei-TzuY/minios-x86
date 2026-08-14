#include "test.h"
#include "fs_conformance.h"
#include "../diskfs.h"
#include "../fs.h"
#include "../ata.h"

#include <string.h>

/*
 * DiskFS is the one filesystem that parses data it did not produce: a
 * superblock and directory read straight off a disk that could hold anything.
 * diskfs_mount() is therefore a trust boundary, and most of its validation
 * (parent-chain walks, duplicate detection, size caps) is unreachable from the
 * shell -- you cannot hand the running kernel a deliberately corrupt disk.
 *
 * Backing ATA with a RAM array makes all of it reachable: lay down a valid
 * filesystem through the normal API, corrupt specific on-disk bytes, and check
 * the mount refuses. The intricate sector read-modify-write in the write path
 * (hole zero-filling, partial sectors) is exercised directly too.
 */

#define STUB_SECTORS 2048

static uint8_t disk[STUB_SECTORS * ATA_SECTOR_SIZE];
static int ata_present = 1;

int ata_is_available(void) { return ata_present; }
uint32_t ata_get_sector_count(void) { return ata_present ? STUB_SECTORS : 0; }

int ata_read_sector(uint32_t lba, uint8_t *buffer) {
    if (!ata_present || !buffer || lba >= STUB_SECTORS) return 0;
    memcpy(buffer, disk + (size_t)lba * ATA_SECTOR_SIZE, ATA_SECTOR_SIZE);
    return 1;
}

int ata_write_sector(uint32_t lba, const uint8_t *buffer) {
    if (!ata_present || !buffer || lba >= STUB_SECTORS) return 0;
    memcpy(disk + (size_t)lba * ATA_SECTOR_SIZE, buffer, ATA_SECTOR_SIZE);
    return 1;
}

/* On-disk layout, mirrored here on purpose: these tests should fail if the
 * format ever changes, rather than quietly following it. */
#define SB_LBA        4
#define DIR_LBA       5
#define ENTRY_SIZE    40
#define FILE_SECTORS  4
#define MAX_FILE_LEN  (FILE_SECTORS * ATA_SECTOR_SIZE)
#define ROOT_PARENT   0xFF
#define ENTRY_FILE    1
#define ENTRY_DIR     2

/* Superblock field offsets. */
#define SB_MAGIC      0
#define SB_VERSION    4
#define SB_TOTAL      12
#define SB_CHECKSUM   36

/* Entry field offsets. */
#define E_USED        0
#define E_TYPE        1
#define E_PARENT      2
#define E_NAME        4
#define E_LENGTH      36

static uint8_t *sb_ptr(void)      { return &disk[(size_t)SB_LBA * ATA_SECTOR_SIZE]; }
static uint8_t *entry_ptr(int i)  { return &disk[(size_t)DIR_LBA * ATA_SECTOR_SIZE + (size_t)i * ENTRY_SIZE]; }

static void poke32(uint8_t *base, uint32_t off, uint32_t v) { memcpy(base + off, &v, 4); }
static uint32_t peek32(const uint8_t *base, uint32_t off) {
    uint32_t v; memcpy(&v, base + off, 4); return v;
}

/* Recompute the superblock checksum over the nine fields that precede it,
 * mirroring diskfs_checksum(). Without this, corrupting any single field also
 * invalidates the checksum, so the checksum test would be the only one doing
 * any work and every other field check would go unverified. */
#define DISKFS_CHECKSUM_SEED 0xA5C35A3CU
static void fix_checksum(void) {
    uint8_t *sb = sb_ptr();
    uint32_t v = DISKFS_CHECKSUM_SEED;
    for (uint32_t off = 0; off < SB_CHECKSUM; off += 4) v ^= peek32(sb, off);
    poke32(sb, SB_CHECKSUM, v);
}

/* Lay down a valid filesystem and return with it mounted. */
static void fresh(void) {
    ata_present = 1;
    memset(disk, 0, sizeof(disk));
    diskfs_install();            /* clears node/ref state; mount fails on a blank disk */
    CHECK(diskfs_format());
    CHECK(diskfs_is_mounted());
}

/* --- functional behaviour ------------------------------------------------- */

static void test_format_and_mount(void) {
    TEST("format/mount");
    fresh();
    CHECK_EQ(diskfs_get_generation(), 1);
    CHECK_EQ(diskfs_get_file_count(), 0);

    /* A freshly formatted volume must survive a re-read from disk. */
    CHECK(diskfs_mount());
    CHECK(diskfs_is_mounted());
    CHECK_EQ(diskfs_get_generation(), 1);
}

static void test_file_roundtrip(void) {
    static const uint8_t payload[] = "diskfs roundtrip payload";
    uint8_t buf[64];
    int n;

    TEST("file roundtrip");
    fresh();
    CHECK(diskfs_create_file("a.txt"));
    CHECK_EQ(diskfs_get_file_count(), 1);
    CHECK(!diskfs_create_file("a.txt"));          /* duplicate refused */
    CHECK(!diskfs_create_file("bad/name"));       /* '/' is not allowed */
    CHECK(!diskfs_create_file(""));               /* empty name refused */

    CHECK(diskfs_write_file("a.txt", payload, sizeof(payload) - 1));
    n = diskfs_read_file("a.txt", buf, sizeof(buf));
    CHECK_EQ(n, (int)(sizeof(payload) - 1));
    CHECK_EQ(memcmp(buf, payload, sizeof(payload) - 1), 0);

    /* Data must survive a remount, i.e. it really reached the disk. */
    TEST("survives remount");
    CHECK(diskfs_mount());
    n = diskfs_read_file("a.txt", buf, sizeof(buf));
    CHECK_EQ(n, (int)(sizeof(payload) - 1));
    CHECK_EQ(memcmp(buf, payload, sizeof(payload) - 1), 0);

    TEST("unlink");
    CHECK(diskfs_unlink_file("a.txt"));
    CHECK_EQ(diskfs_get_file_count(), 0);
    CHECK(diskfs_read_file("a.txt", buf, sizeof(buf)) < 0);
}

static void test_write_holes_and_sector_spans(void) {
    fs_node_t *root, *f;
    uint8_t buf[MAX_FILE_LEN];
    uint8_t data[200];
    unsigned i;

    /* The write path works sector by sector, zero-filling the gap when a write
     * starts past the current end and read-modify-writing partial sectors.
     * Both are easy to get subtly wrong and invisible from the shell. */
    TEST("write leaves a zero-filled hole");
    fresh();
    root = diskfs_get_root_node();
    CHECK(root != NULL);
    if (!root) return;

    f = root->create(root, "h.txt");
    CHECK(f != NULL);
    if (!f) return;

    for (i = 0; i < sizeof(data); i++) data[i] = (uint8_t)(i + 1);
    CHECK_EQ(write_fs(f, 100, sizeof(data), data), sizeof(data));
    CHECK_EQ(f->length, 100 + sizeof(data));

    memset(buf, 0xAA, sizeof(buf));
    CHECK_EQ(read_fs(f, 0, 100 + sizeof(data), buf), 100 + sizeof(data));
    for (i = 0; i < 100; i++) CHECK_EQ(buf[i], 0);            /* the hole */
    for (i = 0; i < sizeof(data); i++) CHECK_EQ(buf[100 + i], data[i]);

    TEST("write across a sector boundary");
    f = root->create(root, "s.txt");
    CHECK(f != NULL);
    if (!f) return;
    /* Straddles the 512-byte boundary, so two sectors are touched. */
    CHECK_EQ(write_fs(f, 450, sizeof(data), data), sizeof(data));
    CHECK_EQ(read_fs(f, 450, sizeof(data), buf), sizeof(data));
    for (i = 0; i < sizeof(data); i++) CHECK_EQ(buf[i], data[i]);

    TEST("size cap enforced");
    /* A file spans FILE_SECTORS sectors and no more. */
    CHECK_EQ(write_fs(f, MAX_FILE_LEN - 10, 100, data), 0);
    CHECK_EQ(write_fs(f, MAX_FILE_LEN, 1, data), 0);
}

static void test_hole_does_not_leak_old_data(void) {
    fs_node_t *root, *f;
    uint8_t buf[MAX_FILE_LEN];
    uint8_t filler[ATA_SECTOR_SIZE];
    unsigned i;

    /*
     * Slots are reused, and a slot's sectors keep the previous file's bytes.
     * When a write starts beyond the current end, the sector is read back from
     * disk (because part of it is already live) and the gap has to be zeroed
     * explicitly -- otherwise the new file would serve the old file's data.
     *
     * Writing into a *fresh* file cannot catch this: that path memsets the
     * whole sector buffer anyway, so the zero-fill is redundant there. The
     * hole only matters once the sector is genuinely read back.
     */
    TEST("hole does not leak a previous file's data");
    fresh();
    root = diskfs_get_root_node();
    CHECK(root != NULL);
    if (!root) return;

    for (i = 0; i < sizeof(filler); i++) filler[i] = 0xEE;

    /* Fill slot 0 with recognisable bytes, then release it. */
    f = root->create(root, "old.txt");
    CHECK(f != NULL);
    if (!f) return;
    CHECK_EQ(write_fs(f, 0, sizeof(filler), filler), sizeof(filler));
    CHECK_EQ(root->unlink(root, "old.txt"), 0);

    /* The new file lands in the same slot, over the same sectors. */
    f = root->create(root, "new.txt");
    CHECK(f != NULL);
    if (!f) return;

    /* Make the first sector partly live, so the next write reads it back... */
    CHECK_EQ(write_fs(f, 0, 10, filler), 10);
    /* ...then write past the end, leaving [10, 300) as a hole. */
    CHECK_EQ(write_fs(f, 300, 10, filler), 10);

    CHECK_EQ(read_fs(f, 0, 310, buf), 310);
    for (i = 10; i < 300; i++) CHECK_EQ(buf[i], 0);   /* must not be 0xEE */
}

static void test_directories(void) {
    fs_node_t *root, *dir, *f;
    uint8_t buf[64];
    static const uint8_t payload[] = "nested";

    TEST("directories");
    fresh();
    root = diskfs_get_root_node();
    CHECK_EQ(root->mkdir(root, "d"), 0);
    dir = root->finddir(root, "d");
    CHECK(dir != NULL);
    if (!dir) return;
    CHECK_EQ(dir->flags, FS_DIRECTORY);

    f = dir->create(dir, "n.txt");
    CHECK(f != NULL);
    if (!f) return;
    CHECK_EQ(write_fs(f, 0, sizeof(payload) - 1, (uint8_t *)payload),
             sizeof(payload) - 1);

    /* A file inside a directory must not be visible from the root. */
    CHECK(root->finddir(root, "n.txt") == NULL);
    CHECK(dir->finddir(dir, "n.txt") != NULL);

    TEST("rmdir refuses a non-empty directory");
    CHECK_EQ(root->rmdir(root, "d"), -1);
    CHECK_EQ(dir->unlink(dir, "n.txt"), 0);
    CHECK_EQ(root->rmdir(root, "d"), 0);
    CHECK(root->finddir(root, "d") == NULL);

    (void)buf;
}

static void test_open_blocks_removal(void) {
    fs_node_t *root, *f;

    TEST("open blocks removal");
    fresh();
    root = diskfs_get_root_node();
    f = root->create(root, "o.txt");
    CHECK(f != NULL);
    if (!f) return;

    open_fs(f);
    CHECK_EQ(root->unlink(root, "o.txt"), -1);   /* in use */
    /* A mount would swap the node table underneath the open descriptor. */
    CHECK_EQ(diskfs_mount(), 0);
    close_fs(f);
    CHECK_EQ(root->unlink(root, "o.txt"), 0);
}

/* --- rejecting corrupt on-disk data --------------------------------------- */

static void expect_mount_refused(const char *what) {
    TEST(what);
    CHECK_EQ(diskfs_mount(), 0);
    CHECK_EQ(diskfs_is_mounted(), 0);
}

static void test_superblock_validation(void) {
    /* Each field is corrupted with the checksum repaired afterwards, so the
     * field's own check is what has to reject the disk -- otherwise every case
     * here would merely be re-testing the checksum. */
    fresh();
    poke32(sb_ptr(), SB_MAGIC, 0xDEADBEEF);
    fix_checksum();
    expect_mount_refused("reject bad magic (checksum repaired)");

    fresh();
    poke32(sb_ptr(), SB_VERSION, 99);
    fix_checksum();
    expect_mount_refused("reject wrong version (checksum repaired)");

    fresh();
    /* Claiming a different disk size than the device reports. */
    poke32(sb_ptr(), SB_TOTAL, STUB_SECTORS * 2);
    fix_checksum();
    expect_mount_refused("reject sector-count mismatch");

    /* And the checksum itself must be enforced: a field left intact but a
     * wrong checksum has to be rejected too. */
    fresh();
    poke32(sb_ptr(), SB_CHECKSUM, 0x12345678);
    expect_mount_refused("reject bad checksum");

    fresh();
    memset(sb_ptr(), 0, ATA_SECTOR_SIZE);
    expect_mount_refused("reject blank superblock");
}

static void test_entry_validation(void) {
    fs_node_t *root;

    /* Corrupting a directory entry leaves the superblock checksum intact, so
     * these exercise the entry validation specifically. */

    TEST("setup two entries");
    fresh();
    root = diskfs_get_root_node();
    CHECK_EQ(root->mkdir(root, "d"), 0);          /* entry 0: directory */
    CHECK(diskfs_create_file("f.txt"));           /* entry 1: file */

    /* An entry that is its own parent must not send the walk into a loop. */
    entry_ptr(1)[E_PARENT] = 1;
    expect_mount_refused("reject self-parented entry");

    fresh();
    root = diskfs_get_root_node();
    CHECK_EQ(root->mkdir(root, "d"), 0);
    CHECK_EQ(root->mkdir(root, "e"), 0);
    entry_ptr(0)[E_PARENT] = 1;                   /* d -> e */
    entry_ptr(1)[E_PARENT] = 0;                   /* e -> d, a cycle */
    expect_mount_refused("reject parent cycle");

    fresh();
    root = diskfs_get_root_node();
    CHECK(diskfs_create_file("f.txt"));           /* entry 0: a file */
    CHECK(diskfs_create_file("g.txt"));           /* entry 1 */
    entry_ptr(1)[E_PARENT] = 0;                   /* parent is a FILE */
    expect_mount_refused("reject file used as a parent");

    fresh();
    CHECK(diskfs_create_file("f.txt"));
    entry_ptr(0)[E_PARENT] = 99;                  /* out of range, not root */
    expect_mount_refused("reject out-of-range parent");

    fresh();
    CHECK(diskfs_create_file("f.txt"));
    CHECK(diskfs_create_file("g.txt"));
    memcpy(entry_ptr(1) + E_NAME, "f.txt", 6);    /* same name, same parent */
    expect_mount_refused("reject duplicate names");

    fresh();
    CHECK(diskfs_create_file("f.txt"));
    poke32(entry_ptr(0), E_LENGTH, MAX_FILE_LEN + 1);
    expect_mount_refused("reject oversized file length");

    fresh();
    root = diskfs_get_root_node();
    CHECK_EQ(root->mkdir(root, "d"), 0);
    poke32(entry_ptr(0), E_LENGTH, 512);          /* directories have no length */
    expect_mount_refused("reject directory with a length");

    fresh();
    CHECK(diskfs_create_file("f.txt"));
    entry_ptr(0)[E_TYPE] = 7;                     /* neither file nor directory */
    expect_mount_refused("reject unknown entry type");

    fresh();
    CHECK(diskfs_create_file("f.txt"));
    entry_ptr(0)[E_USED] = 2;                     /* used must be exactly 1 */
    expect_mount_refused("reject bad used flag");

    fresh();
    CHECK(diskfs_create_file("f.txt"));
    memset(entry_ptr(0) + E_NAME, 'A', 32);       /* no NUL terminator */
    expect_mount_refused("reject unterminated name");

    fresh();
    CHECK(diskfs_create_file("f.txt"));
    entry_ptr(0)[E_NAME] = '\0';                  /* empty name */
    expect_mount_refused("reject empty name");
}

static void test_no_device(void) {
    static const uint8_t payload[] = "still here";
    uint8_t buf[32];

    TEST("no device");
    fresh();
    CHECK(diskfs_create_file("x.txt"));
    CHECK(diskfs_write_file("x.txt", payload, sizeof(payload) - 1));

    ata_present = 0;

    /* Both refuse -- and both deliberately leave the existing mount in place.
     * "Cannot mount now" is not the same as "unmount": the identical early
     * return also covers the open-descriptor case, where tearing the mount
     * down would strand live nodes. test_open_blocks_removal relies on that,
     * so this is load-bearing behaviour rather than an oversight. */
    CHECK_EQ(diskfs_mount(), 0);
    CHECK_EQ(diskfs_format(), 0);
    CHECK(diskfs_is_mounted());

    /* Actual I/O does fail, since every read goes through the device. */
    CHECK(diskfs_read_file("x.txt", buf, sizeof(buf)) < 0);

    ata_present = 1;
    CHECK(diskfs_mount());
    CHECK_EQ(diskfs_read_file("x.txt", buf, sizeof(buf)),
             (int)(sizeof(payload) - 1));
}


/* The shared contract every backend owes (see tests/fs_conformance.h).
 * DiskFS caps a file at DISKFS_FILE_SECTORS sectors, four orders of magnitude
 * below what sys_seek will hand it, so every offset the contract uses is
 * unreachable here by a wide margin. */
static void test_backend_conformance(void) {
    static const uint8_t content[] = { 'd', 'i', 's', 'k' };
    fs_node_t *root;
    fs_node_t *f;

    diskfs_format();
    CHECK(diskfs_mount());
    root = diskfs_get_root_node();
    CHECK(root != NULL);
    if (!root) return;

    f = root->create(root, "conf.txt");
    CHECK(f != NULL);
    if (!f) return;
    CHECK_EQ(f->write(f, 0, sizeof(content), (uint8_t *)content),
             sizeof(content));

    fs_conformance_extreme_offsets(f, content, sizeof(content), "diskfs");
}

int main(void) {
    fs_conformance_arm_watchdog(30);
    test_format_and_mount();
    test_file_roundtrip();
    test_write_holes_and_sector_spans();
    test_hole_does_not_leak_old_data();
    test_directories();
    test_open_blocks_removal();
    test_superblock_validation();
    test_entry_validation();
    test_no_device();
    test_backend_conformance();
    TEST_REPORT("diskfs");
}
