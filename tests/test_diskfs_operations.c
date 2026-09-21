/* DiskFS + real VFS, with suspended hosted stacks at ATA completion and
 * scheduler waits. The CPU mutex models ONE processor, not an FS lock: every
 * context switch releases it, including a device sleep with IF=0. Tests never
 * access DiskFS's private lock or metadata. */
#include "test.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define IRQ_H
static uint32_t save_irq_disable(void);
static void restore_irq(uint32_t flags);
#include "../diskfs.c"

#define SECTORS 128
#define IF_FLAG (1U << 9)
static uint8_t media[SECTORS][ATA_SECTOR_SIZE];
static pthread_mutex_t cpu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event = PTHREAD_COND_INITIALIZER;
static __thread int actor = -1;
static __thread uint32_t irq_flags;

typedef struct {
    pthread_t thread;
    int started, done, blocked, wakes, blocks, delay;
    const void *channel;
    void (*run)(int);
    int result;
    uint32_t entry_flags;
    uint8_t buffer[1024];
} worker_t;
static worker_t workers[2];
static int pause_write, pause_lba, paused, resume_io, preempt;
static int write_calls, fail_write, io_calls;
static int read_calls, fail_read;
static int delay_waiter;
static fs_node_t *file;
static uint8_t original[1024], replacement[1024];

static void wait_event(void) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;
    if (pthread_cond_timedwait(&event, &cpu, &deadline) != 0) {
        CHECK(0 && "operation/wakeup liveness: no progress");
        exit(1);
    }
}

static uint32_t save_irq_disable(void) {
    uint32_t flags = irq_flags;
    irq_flags = 0;
    return flags;
}

static void restore_irq(uint32_t flags) { irq_flags = flags; }

void task_block_current(const void *channel) {
    CHECK_EQ(irq_flags, 0);
    if (actor < 0) {
        CHECK(0 && "reference cleanup must not sleep");
        exit(1);
    }
    worker_t *w = &workers[actor];
    w->channel = channel;
    w->blocked = 1;
    w->blocks++;
    pthread_cond_broadcast(&event);
    while (w->blocked || w->delay) wait_event();
    w->channel = NULL;
}

void task_wake_one(const void *channel) {
    CHECK_EQ(irq_flags, 0);
    for (int i = 0; i < 2; i++) {
        if (workers[i].blocked && workers[i].channel == channel) {
            workers[i].blocked = 0;
            workers[i].wakes++;
            pthread_cond_broadcast(&event);
            break;
        }
    }
}

/* Killing a waiter must not discard the caller's resources or the owner's
 * operation. Model a kill wake as well as an ordinary spurious wake. */
int task_kill_pending(void) { return actor == 1; }
void task_exit(int32_t status) {
    (void)status;
    CHECK(0 && "DiskFS must return through caller cleanup");
    exit(1);
}

int ata_is_available(void) { return 1; }
uint32_t ata_get_sector_count(void) { return SECTORS; }

static void device_pause(int writing, uint32_t lba) {
    if (actor != 0 || paused || writing != pause_write ||
        (int)lba != pause_lba) return;
    /* With preempt=1 this is the current PIO driver's restore-IF boundary;
     * otherwise it is a future blocking device wait, entered with IF clear. */
    CHECK_EQ(irq_flags, preempt ? IF_FLAG : 0);
    paused = 1;
    pthread_cond_broadcast(&event);
    while (!resume_io) wait_event();
}

int ata_read_sector(uint32_t lba, uint8_t *buffer) {
    uint32_t flags = save_irq_disable();
    CHECK(lba < SECTORS);
    io_calls++;
    read_calls++;
    memcpy(buffer, media[lba], ATA_SECTOR_SIZE);
    if (!preempt) device_pause(0, lba);
    restore_irq(flags);
    if (preempt) device_pause(0, lba);
    return !fail_read || read_calls != fail_read;
}

int ata_write_sector(uint32_t lba, const uint8_t *buffer) {
    uint32_t flags = save_irq_disable();
    CHECK(lba < SECTORS);
    io_calls++;
    write_calls++;
    if (!preempt) device_pause(1, lba);
    int fail = fail_write && write_calls == fail_write;
    if (!fail) memcpy(media[lba], buffer, ATA_SECTOR_SIZE);
    restore_irq(flags);
    if (preempt) device_pause(1, lba);
    return !fail;
}

static void *run_worker(void *arg) {
    actor = (int)(intptr_t)arg;
    pthread_mutex_lock(&cpu);
    worker_t *w = &workers[actor];
    irq_flags = w->entry_flags;
    w->started = 1;
    w->run(actor);
    CHECK_EQ(irq_flags, w->entry_flags);
    w->done = 1;
    pthread_cond_broadcast(&event);
    pthread_mutex_unlock(&cpu);
    return NULL;
}

static void start_worker(int id, void (*run)(int), uint32_t flags) {
    workers[id].run = run;
    workers[id].entry_flags = flags;
    CHECK_EQ(pthread_create(&workers[id].thread, NULL, run_worker,
                            (void *)(intptr_t)id), 0);
}

static void fresh(void) {
    memset(workers, 0, sizeof(workers));
    memset(media, 0, sizeof(media));
    pause_lba = -1;
    paused = resume_io = preempt = write_calls = fail_write = io_calls = 0;
    read_calls = fail_read = 0;
    delay_waiter = 0;
    irq_flags = 0;
    diskfs_install();
    CHECK(diskfs_format());
    fs_root = diskfs_get_root_node();
    CHECK(fs_root != NULL);
    file = create_fs("/file");
    CHECK(file != NULL);
    for (unsigned i = 0; i < sizeof(original); i++) {
        original[i] = (uint8_t)(i * 13 + 7);
        replacement[i] = (uint8_t)(i * 3 + 19);
    }
    CHECK_EQ(write_fs(file, 0, sizeof(original), original), sizeof(original));
    write_calls = 0;
    read_calls = 0;
}

static void overlap(void (*first)(int), void (*second)(int), int writing,
                    int lba, int preemption, int spurious) {
    pause_write = writing;
    pause_lba = lba;
    preempt = preemption;
    start_worker(0, first, preemption ? IF_FLAG : 0);
    while (!paused && !workers[0].done) wait_event();
    CHECK(paused);
    workers[1].delay = delay_waiter;
    start_worker(1, second, 0);
    while (!workers[1].blocked && !workers[1].done) wait_event();
    CHECK(!workers[1].done); /* no intermediate operation result is published */
    if (spurious && workers[1].blocked) {
        int blocks = workers[1].blocks;
        workers[1].blocked = 0;
        pthread_cond_broadcast(&event);
        while (workers[1].blocks == blocks && !workers[1].done) wait_event();
        CHECK(!workers[1].done);
    }
    /* Duplicate/close may also run on a retired task's stack. They must finish
     * without sleeping, even while another task owns the operation gate. */
    if (file && file->open) {
        open_fs(file);
        close_fs(file);
    }
    if (delay_waiter) close_fs(file); /* sibling drops the last descriptor */
    resume_io = 1;
    pthread_cond_broadcast(&event);
    if (delay_waiter) {
        while (!workers[0].done) wait_event();
        /* The waiter is runnable but has not regained the CPU. A new caller
         * may acquire the now-free gate first; its node must still be pinned. */
        CHECK_EQ(unlink_fs("/file"), -1);
        CHECK_EQ(diskfs_mount(), 0);
        workers[1].delay = 0;
        pthread_cond_broadcast(&event);
    }
    while (!workers[0].done || !workers[1].done) wait_event();
    pthread_mutex_unlock(&cpu);
    for (int i = 0; i < 2; i++) CHECK_EQ(pthread_join(workers[i].thread, NULL), 0);
    pthread_mutex_lock(&cpu);
    pause_lba = -1;
    preempt = 0;
}

static int reverse;
static void patch_sector(int id) {
    int offset = (id ^ reverse) ? 97 : 17;
    workers[id].result = (int)write_fs(file, offset, 7, replacement + offset);
}

static void read_whole(int id) {
    workers[id].result = (int)read_fs(file, 0, sizeof(original), workers[id].buffer);
}

static void replace_whole(int id) {
    workers[id].result = diskfs_write_file("file", replacement, sizeof(replacement));
}

static void vfs_replace_whole(int id) {
    workers[id].result = write_fs(file, 0, sizeof(replacement), replacement) ==
                         sizeof(replacement);
}

static void test_data_operations(void) {
    for (int mode = 0; mode < 2; mode++) {
        for (reverse = 0; reverse < 2; reverse++) {
            TEST("overlapping partial writes preserve both byte ranges");
            fresh();
            open_fs(file);
            overlap(patch_sector, patch_sector, 0, 16, mode, 1);
            CHECK_EQ(workers[0].result, 7);
            CHECK_EQ(workers[1].result, 7);
            memcpy(original + 17, replacement + 17, 7);
            memcpy(original + 97, replacement + 97, 7);
            close_fs(file);
            CHECK(diskfs_mount());
            CHECK_EQ(diskfs_read_file("file", workers[0].buffer, 1024), 1024);
            CHECK_EQ(memcmp(workers[0].buffer, original, 1024), 0);
        }
        TEST("multi-sector read observes one complete file version");
        fresh();
        open_fs(file);
        overlap(read_whole, replace_whole, 0, 16, mode, 0);
        CHECK_EQ(workers[0].result, 1024);
        CHECK_EQ(workers[1].result, 1);
        CHECK_EQ(memcmp(workers[0].buffer, original, 1024), 0);
        close_fs(file);
        CHECK(diskfs_mount());
        CHECK_EQ(diskfs_read_file("file", workers[0].buffer, 1024), 1024);
        CHECK_EQ(memcmp(workers[0].buffer, replacement, 1024), 0);
    }
}

static void create_a(int id) { workers[id].result = create_fs("/new-a") != NULL; }
static void create_b(int id) { workers[id].result = diskfs_create_file("new-b"); }
static void lookup_a(int id) {
    fs_node_t *node = resolve_fs("/new-a");
    workers[id].result = node && node->flags == FS_FILE;
}
static void reuse_slot(int id) {
    workers[id].result = unlink_fs("/file") == 0 &&
        create_fs("/new-b") != NULL &&
        diskfs_write_file("new-b", original, sizeof(original));
}
static void mount_volume(int id) { workers[id].result = diskfs_mount(); }
static void format_volume(int id) { workers[id].result = diskfs_format(); }
static void close_and_unlink(int id) {
    close_fs(file);
    workers[id].result = unlink_fs("/file");
}

static void test_namespace_operations(void) {
    for (int writing = 0; writing < 2; writing++) {
        TEST(writing ? "queued write pins its node across close and overtaking removal" :
                       "queued read pins its node across close and overtaking removal");
        fresh();
        open_fs(file);
        delay_waiter = 1;
        overlap(replace_whole, writing ? vfs_replace_whole : read_whole, 1, 16, 0, 0);
        CHECK_EQ(workers[0].result, 1);
        CHECK_EQ(workers[1].result, writing ? 1 : 1024);
        CHECK_EQ(diskfs_read_file("file", workers[0].buffer, 1024), 1024);
        CHECK_EQ(memcmp(workers[0].buffer, replacement, 1024), 0);
        CHECK_EQ(unlink_fs("/file"), 0); /* neither operation leaked a pin */
    }

    TEST("active read pins its node after a sibling closes the descriptor");
    fresh();
    open_fs(file);
    overlap(read_whole, close_and_unlink, 0, 16, 0, 0);
    /* Removal waits until the active read has returned, then succeeds. */
    CHECK_EQ(workers[0].result, 1024);
    CHECK_EQ(memcmp(workers[0].buffer, original, 1024), 0);
    CHECK_EQ(workers[1].result, 0);
    CHECK_EQ(diskfs_get_file_count(), 0);

    TEST("lookup waits for node publication");
    fresh();
    overlap(create_a, lookup_a, 1, 5, 0, 0);
    CHECK_EQ(workers[0].result, 1);
    CHECK_EQ(workers[1].result, 1);

    TEST("concurrent creates persist both directory entries");
    fresh();
    overlap(create_a, create_b, 1, 5, 0, 0);
    CHECK_EQ(workers[0].result, 1);
    CHECK_EQ(workers[1].result, 1);
    CHECK(diskfs_mount());
    CHECK(resolve_fs("/new-a") != NULL);
    CHECK(resolve_fs("/new-b") != NULL);
    CHECK_EQ(diskfs_get_file_count(), 3);
    CHECK_EQ(diskfs_get_generation(), 5);

    TEST("unlink and slot reuse cannot overtake an active direct write");
    fresh();
    overlap(replace_whole, reuse_slot, 1, 16, 0, 0);
    CHECK_EQ(workers[0].result, 1);
    CHECK_EQ(workers[1].result, 1);
    CHECK(diskfs_mount());
    CHECK_EQ(diskfs_read_file("new-b", workers[0].buffer, 1024), 1024);
    CHECK_EQ(memcmp(workers[0].buffer, original, 1024), 0);

    TEST("remount and format wait for I/O then honour live references");
    for (int format = 0; format < 2; format++) {
        fresh();
        open_fs(file);
        overlap(replace_whole, format ? format_volume : mount_volume, 1, 16, 0, 0);
        CHECK_EQ(workers[0].result, 1);
        CHECK_EQ(workers[1].result, 0);
        close_fs(file);
        CHECK(diskfs_mount());
        CHECK_EQ(diskfs_read_file("file", workers[0].buffer, 1024), 1024);
        CHECK_EQ(memcmp(workers[0].buffer, replacement, 1024), 0);
    }
}

static void test_failure_cleanup(int vfs) {
    /* Two data sectors, two directory sectors, then the superblock. Each
     * failure must release ownership AND prevent retained nodes serving the
     * partial operation from the old cache. Physical rollback is not promised. */
    for (int failure = 1; failure <= 5; failure++) {
        TEST("failed write releases waiters and invalidates cached I/O");
        fresh();
        open_fs(file);
        fail_write = failure;
        overlap(vfs ? vfs_replace_whole : replace_whole, read_whole, 1, 16, 0, 0);
        CHECK_EQ(workers[0].result, 0);
        CHECK_EQ(workers[1].result, 0);
        CHECK_EQ(diskfs_is_mounted(), 0);
        int calls = io_calls;
        CHECK_EQ(read_fs(file, 0, 1, workers[0].buffer), 0);
        CHECK_EQ(write_fs(file, 0, 1, replacement), 0);
        CHECK_EQ(io_calls, calls);
        CHECK_EQ(diskfs_mount(), 0); /* failed mount still has a live reference */
        close_fs(file);
        fail_write = 0;
        CHECK(diskfs_format());
        CHECK(diskfs_create_file("recovery"));
        CHECK(diskfs_mount());
        CHECK_EQ(diskfs_get_file_count(), 1);
    }
}

static void test_rmw_read_failure(void) {
    for (int failure = 1; failure <= 2; failure++) {
        TEST("read-modify-write failure releases ownership and pins");
        fresh();
        open_fs(file);
        fail_read = failure;
        overlap(vfs_replace_whole, read_whole, 0, 15 + failure, 0, 0);
        CHECK_EQ(workers[0].result, 0);
        CHECK_EQ(workers[1].result, 0);
        CHECK_EQ(diskfs_is_mounted(), 0);
        close_fs(file);
        fail_read = 0;
        CHECK(diskfs_format());
    }
}

int main(void) {
    pthread_mutex_lock(&cpu);
    test_data_operations();
    test_namespace_operations();
    test_failure_cleanup(0);
    test_failure_cleanup(1);
    test_rmw_read_failure();
    pthread_mutex_unlock(&cpu);
    TEST_REPORT("diskfs-ops");
}
