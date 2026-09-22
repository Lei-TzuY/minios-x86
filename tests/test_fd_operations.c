/* Real syscall -> VFS -> DiskFS operations on one simulated CPU. Pthread
 * stacks suspend at device/scheduler boundaries; the CPU mutex is released at
 * each suspension, even with IF clear. Assertions observe bytes, offsets,
 * references (unlink), return values and progress, never private lock fields. */
#include "test.h"
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>

#define IRQ_H
static uint32_t save_irq_disable(void);
static void restore_irq(uint32_t flags);
#include "../syscall.c"
#include "../diskfs.c"
#include "../ramfs.h"

#define WORKERS 3
#define SECTORS 128
#define IF_FLAG (1U << 9)
#define USER_PAGE 4096
static pthread_mutex_t cpu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t event = PTHREAD_COND_INITIALIZER;
static __thread int actor = -1;
static __thread uint32_t irq_flags;
static process_t procs[2];
static int controller_process;
static uint8_t media[SECTORS][ATA_SECTOR_SIZE];
static char *user_pages;
static int mapped[WORKERS];
static int fd_a, fd_b, pause_actor, pause_write, paused, resume_io, preempt;
static int fail_write, write_calls, live_allocs;
static uint8_t seed[1024];

typedef struct {
    pthread_t thread;
    void (*run)(int);
    int done, blocked, blocks, delay, killed, process;
    const void *channel;
    uint32_t flags;
    int result;
} worker_t;
static worker_t workers[WORKERS];

static char *buffer(int id) { return user_pages + id * USER_PAGE + 256; }
static char *path(const char *value) { strcpy(user_pages, value); return user_pages; }
/* Default devices cannot do real terminal/keyboard I/O on the host. Keep their
 * dispatch observable; redirected operations use the real filesystems/pipes. */
static int terminal_bytes, keyboard_calls, keyboard_wait;
void terminal_write(const char *data, size_t count) {
    (void)data;
    terminal_bytes += (int)count;
}
size_t keyboard_read(char *data, size_t count) {
    keyboard_calls++;
    if (!count) return 0;
    if (keyboard_wait) task_block_current(&keyboard_wait);
    data[0] = 'K';
    return 1;
}

/* Model the nonblocking standard-stream part of final process cleanup. The
 * indexed table cleanup below is the real syscall lifecycle hook. */
static void close_streams(process_t *p) {
    if (p->stdin_node) close_fs(p->stdin_node);
    if (p->stdout_node) close_fs(p->stdout_node);
    if (p->stdin_pipe) pipe_close_read(p->stdin_pipe);
    if (p->stdout_pipe) pipe_close_write(p->stdout_pipe);
    p->stdin_node = p->stdout_node = NULL;
    p->stdin_pipe = p->stdout_pipe = NULL;
}
static void wait_event(void) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 5;
    if (pthread_cond_timedwait(&event, &cpu, &deadline) != 0) {
        CHECK(0 && "descriptor operation/wakeup liveness: no progress");
        exit(1);
    }
}
static uint32_t save_irq_disable(void) {
    uint32_t flags = irq_flags;
    irq_flags = 0;
    return flags;
}
static void restore_irq(uint32_t flags) { irq_flags = flags; }

process_t *process_get_current(void) {
    return &procs[actor < 0 ? controller_process : workers[actor].process];
}
int paging_user_range_mapped(uint32_t address, uint32_t size) {
    if (address < USER_EXT_BASE || address >= USER_EXT_BASE + WORKERS * USER_PAGE)
        return 0;
    uint32_t offset = address - USER_EXT_BASE;
    if (size > WORKERS * USER_PAGE - offset) return 0;
    for (uint32_t i = offset / USER_PAGE; size && i <= (offset + size - 1) / USER_PAGE; i++)
        if (!mapped[i]) return 0;
    return 1;
}
void *kmalloc(size_t size) {
    void *result = malloc(size);
    if (result) live_allocs++;
    return result;
}
void kfree(void *ptr) { if (ptr) live_allocs--; free(ptr); }

void task_block_current(const void *channel) {
    CHECK_EQ(irq_flags, 0);
    if (actor < 0) {
        CHECK(0 && "controller operation must make progress without blocking");
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
static void wake(const void *channel, int one) {
    for (int i = 0; i < WORKERS; i++) {
        worker_t *w = &workers[i];
        if (w->blocked && w->channel == channel) {
            w->blocked = 0;
            pthread_cond_broadcast(&event);
            if (one) break;
        }
    }
}
void task_wake_one(const void *channel) { wake(channel, 1); }
void task_wake_all(const void *channel) { wake(channel, 0); }
int task_kill_pending(void) { return actor >= 0 && workers[actor].killed; }
void task_exit(int32_t status) {
    (void)status;
    CHECK(0 && "descriptor waits must return through caller resource cleanup");
    exit(1);
}
int ata_is_available(void) { return 1; }
uint32_t ata_get_sector_count(void) { return SECTORS; }
static void device_pause(int writing) {
    if (actor != pause_actor || paused || writing != pause_write) return;
    CHECK_EQ(irq_flags, preempt ? IF_FLAG : 0);
    paused = 1;
    pthread_cond_broadcast(&event);
    while (!resume_io) wait_event();
}
int ata_read_sector(uint32_t lba, uint8_t *out) {
    CHECK(lba < SECTORS);
    memcpy(out, media[lba], ATA_SECTOR_SIZE);
    device_pause(0);
    return 1;
}
int ata_write_sector(uint32_t lba, const uint8_t *in) {
    CHECK(lba < SECTORS);
    write_calls++;
    device_pause(1);
    if (write_calls == fail_write) return 0;
    memcpy(media[lba], in, ATA_SECTOR_SIZE);
    return 1;
}
static void *run_worker(void *arg) {
    actor = (int)(intptr_t)arg;
    pthread_mutex_lock(&cpu);
    worker_t *w = &workers[actor];
    irq_flags = w->flags;
    w->run(actor);
    CHECK_EQ(irq_flags, w->flags);
    w->done = 1;
    pthread_cond_broadcast(&event);
    pthread_mutex_unlock(&cpu);
    return NULL;
}
static void start(int id, void (*run)(int)) {
    workers[id].run = run;
    CHECK_EQ(pthread_create(&workers[id].thread, NULL, run_worker,
                            (void *)(intptr_t)id), 0);
}
static void stopped(int id) {
    while (!workers[id].blocked && !workers[id].done) wait_event();
}
static void finish(int n) {
    resume_io = 1;
    pthread_cond_broadcast(&event);
    for (int i = 0; i < n; i++) while (!workers[i].done) wait_event();
    pthread_mutex_unlock(&cpu);
    for (int i = 0; i < n; i++) CHECK_EQ(pthread_join(workers[i].thread, NULL), 0);
    pthread_mutex_lock(&cpu);
    pause_actor = -2;
}
static void first(void (*run)(int), int writing) {
    pause_actor = 0;
    pause_write = writing;
    start(0, run);
    while (!paused && !workers[0].done) wait_event();
    CHECK(paused);
}
static void fresh(void) {
    pause_actor = -2;
    controller_process = 0;
    close_streams(&procs[0]);
    close_streams(&procs[1]);
    syscall_close_user_files(&procs[0]);
    syscall_close_user_files(&procs[1]);
    memset(procs, 0, sizeof(procs));
    procs[1].slot = 1;
    memset(workers, 0, sizeof(workers));
    memset(media, 0, sizeof(media));
    memset(user_pages, 0, WORKERS * USER_PAGE);
    for (int i = 0; i < WORKERS; i++) mapped[i] = 1;
    paused = resume_io = preempt = write_calls = fail_write = 0;
    terminal_bytes = keyboard_calls = keyboard_wait = 0;
    irq_flags = 0;
    diskfs_install();
    CHECK(diskfs_format());
    CHECK(diskfs_create_file("a"));
    CHECK(diskfs_create_file("b"));
    CHECK(diskfs_create_file("guard"));
    CHECK(diskfs_write_file("a", seed, sizeof(seed)));
    CHECK(diskfs_write_file("b", seed, sizeof(seed)));
    fd_a = sys_open(path("/disk/a"));
    fd_b = sys_open(path("/disk/b"));
    CHECK_EQ(fd_a, 3);
    CHECK_EQ(fd_b, 4);
    for (int i = 0; i < WORKERS; i++) memset(buffer(i), 'A' + i, 32);
    CHECK_EQ(write_fs(ramfs_find_file("/ram"), 0, 32, seed + 17), 32);
    write_calls = 0;
}
static void read_a(int id) { workers[id].result = sys_read_file(fd_a, buffer(id), 7); }
static void write_a(int id) { workers[id].result = sys_write_file(fd_a, buffer(id), 7); }
static void write_b(int id) { workers[id].result = sys_write_file(fd_b, buffer(id), 7); }
static void seek_a(int id) { workers[id].result = sys_seek(fd_a, 20, SYS_SEEK_CUR); }
static void stat_a(int id) { workers[id].result = sys_fstat(fd_a, buffer(id)); }
static void close_a(int id) { workers[id].result = sys_close(fd_a); }
static void dup_a(int id) { workers[id].result = sys_dup(fd_a); }
static void dup_ab(int id) { workers[id].result = sys_dup2(fd_a, fd_b); }
static void dup_ba(int id) { workers[id].result = sys_dup2(fd_b, fd_a); }
static void copy_files(int id) {
    syscall_copy_user_files(&procs[0], &procs[1]);
    workers[id].process = 1;
    workers[id].result = sys_seek(fd_a, 0, SYS_SEEK_CUR);
}
static void close_reopen(int id) {
    CHECK_EQ(sys_close(fd_a), 0);
    strcpy(buffer(id), "/ram");
    workers[id].result = sys_open(buffer(id));
}
static void kernel_write(int id) {
    workers[id].result = diskfs_write_file("guard", seed, 7);
}
static void ram_write(int id) {
    strcpy(buffer(id), "/ram");
    int fd = sys_open(buffer(id));
    CHECK(fd >= 0);
    workers[id].result = sys_write_file(fd, buffer(id), 4);
    CHECK_EQ(sys_close(fd), 0);
}
static void check_bytes(const char *name, const uint8_t *expected, unsigned size) {
    uint8_t got[1024];
    CHECK_EQ(diskfs_read_file(name, got, size), size);
    CHECK(memcmp(got, expected, size) == 0);
}
static void test_shared_offsets(void) {
    for (int reverse = 0; reverse < 2; reverse++) {
        TEST("same descriptor writes commit distinct consecutive byte ranges");
        fresh();
        memset(buffer(0), reverse ? 'B' : 'A', 7);
        memset(buffer(1), reverse ? 'A' : 'B', 7);
        first(write_a, 1);
        start(1, write_a);
        stopped(1);
        finish(2);
        uint8_t expected[14];
        memcpy(expected, buffer(0), 7);
        memcpy(expected + 7, buffer(1), 7);
        check_bytes("a", expected, sizeof(expected));
        CHECK_EQ(sys_seek(fd_a, 0, SYS_SEEK_CUR), 14);
        CHECK_EQ(workers[0].result, 7);
        CHECK_EQ(workers[1].result, 7);
    }
    TEST("same descriptor reads consume different bytes after a device sleep");
    fresh();
    first(read_a, 0);
    start(1, read_a);
    stopped(1);
    finish(2);
    CHECK(memcmp(buffer(0), seed, 7) == 0);
    CHECK(memcmp(buffer(1), seed + 7, 7) == 0);
    CHECK_EQ(sys_seek(fd_a, 0, SYS_SEEK_CUR), 14);
}
static void test_current_pio_boundary(void) {
    TEST("preemptible kernel DiskFS owner queues two IF=0 syscall writers");
    fresh();
    preempt = 1;
    workers[0].flags = IF_FLAG;
    first(kernel_write, 1);
    start(1, write_a);
    stopped(1);
    start(2, write_a);
    stopped(2);
    finish(3);
    uint8_t expected[14];
    memcpy(expected, buffer(1), 7);
    memcpy(expected + 7, buffer(2), 7);
    check_bytes("a", expected, sizeof(expected));
    CHECK_EQ(sys_seek(fd_a, 0, SYS_SEEK_CUR), 14);
}
static void test_offset_observers(void) {
    TEST("seek CUR follows the completed I/O offset");
    fresh();
    first(read_a, 0);
    start(1, seek_a);
    stopped(1);
    finish(2);
    CHECK_EQ(workers[1].result, 27);
    CHECK_EQ(sys_seek(fd_a, 0, SYS_SEEK_CUR), 27);

    TEST("dup snapshots the committed offset and has independent ownership");
    fresh();
    first(read_a, 0);
    start(1, dup_a);
    stopped(1);
    finish(2);
    int copy = workers[1].result;
    CHECK(copy >= 0);
    CHECK_EQ(sys_seek(copy, 0, SYS_SEEK_CUR), 7);
    CHECK_EQ(sys_close(fd_a), 0);
    CHECK_EQ(unlink_fs("/disk/a"), -1);
    CHECK_EQ(sys_read_file(copy, buffer(0), 7), 7);
    CHECK(memcmp(buffer(0), seed + 7, 7) == 0);
    CHECK_EQ(sys_close(copy), 0);
    CHECK_EQ(unlink_fs("/disk/a"), 0);

    TEST("fork table copy waits for committed offset without inheriting locks");
    fresh();
    first(read_a, 0);
    start(1, copy_files);
    stopped(1);
    finish(2);
    CHECK_EQ(workers[1].result, 7);
    CHECK_EQ(sys_seek(fd_a, 0, SYS_SEEK_CUR), 7);
    /* Deliberately use the other current process, as retired-stack cleanup does. */
    controller_process = 1;
    syscall_close_user_files(&procs[0]);
    CHECK_EQ(sys_read_file(fd_a, buffer(0), 7), 7);
    CHECK(memcmp(buffer(0), seed + 7, 7) == 0);
    syscall_close_user_files(&procs[1]);
    CHECK_EQ(unlink_fs("/disk/a"), 0);
    controller_process = 0;

    TEST("fstat observes a completed size update");
    fresh();
    CHECK_EQ(sys_seek(fd_a, 1100, SYS_SEEK_SET), 1100);
    first(write_a, 1);
    start(1, stat_a);
    stopped(1);
    finish(2);
    CHECK_EQ(workers[1].result, 0);
    CHECK_EQ(((uint32_t *)buffer(1))[0], 1107);
}
static void test_replacement(void) {
    TEST("close/reopen cannot apply an old I/O offset to the replacement slot");
    fresh();
    first(read_a, 0);
    start(1, close_reopen);
    stopped(1);
    finish(2);
    CHECK_EQ(workers[1].result, fd_a);
    CHECK_EQ(sys_seek(fd_a, 0, SYS_SEEK_CUR), 0);
    CHECK_EQ(sys_read_file(fd_a, buffer(1), 7), 7);
    CHECK(memcmp(buffer(1), seed + 17, 7) == 0);
    CHECK_EQ(unlink_fs("/disk/a"), 0);

    TEST("dup2 replacement waits for destination I/O and retains source offset");
    fresh();
    CHECK_EQ(sys_seek(fd_b, 30, SYS_SEEK_SET), 30);
    first(read_a, 0);
    start(1, dup_ba);
    stopped(1);
    finish(2);
    CHECK_EQ(workers[1].result, fd_a);
    CHECK_EQ(sys_seek(fd_a, 0, SYS_SEEK_CUR), 30);
    CHECK_EQ(sys_read_file(fd_a, buffer(1), 7), 7);
    CHECK(memcmp(buffer(1), seed + 30, 7) == 0);
    CHECK_EQ(sys_seek(fd_b, 0, SYS_SEEK_CUR), 30);
    CHECK_EQ(unlink_fs("/disk/a"), 0);

    TEST("opposite dup2 directions cannot form a descriptor lock cycle");
    fresh();
    first(write_b, 1);
    workers[1].delay = 1;
    start(1, dup_ab);
    stopped(1);
    start(2, dup_ba);
    stopped(2);
    resume_io = 1;
    pthread_cond_broadcast(&event);
    while (!workers[0].done) wait_event();
    /* Let an incorrectly ordered contender acquire its second lock before
     * resuming the first dup2. Correct code still waits for the lower slot. */
    while (!workers[2].blocked && !workers[2].done) wait_event();
    workers[1].delay = 0;
    pthread_cond_broadcast(&event);
    finish(3);
    CHECK_EQ(workers[1].result, fd_b);
    CHECK_EQ(workers[2].result, fd_a);
    CHECK_EQ(sys_seek(fd_a, 0, SYS_SEEK_CUR), 0);
    CHECK_EQ(sys_seek(fd_b, 0, SYS_SEEK_CUR), 0);
    CHECK_EQ(unlink_fs("/disk/b"), 0);
}
static void test_reserved_destination(void) {
    TEST("open and dup skip an empty slot reserved by a waiting dup2");
    fresh();
    CHECK_EQ(sys_close(fd_a), 0);
    first(write_b, 1);
    start(1, dup_ba);
    stopped(1);
    /* The lower destination is empty but already owned while dup2 waits for
     * its source. Publishing into it would violate that ownership. */
    int independent = sys_open(path("/ram"));
    CHECK_EQ(independent, 5);
    int copy = sys_dup(independent);
    CHECK_EQ(copy, 6);
    finish(2);
    CHECK_EQ(workers[1].result, fd_a);
    CHECK_EQ(sys_seek(fd_a, 0, SYS_SEEK_CUR), 7);
    CHECK_EQ(sys_seek(independent, 0, SYS_SEEK_CUR), 0);
    CHECK_EQ(sys_seek(copy, 0, SYS_SEEK_CUR), 0);
    CHECK_EQ(sys_close(independent), 0);
    CHECK_EQ(sys_close(copy), 0);
}
static void test_queued_lookup(void) {
    for (int reuse = 0; reuse < 2; reuse++) {
        TEST("queued read resolves the slot after a competing close or pipe reuse");
        fresh();
        first(read_a, 0);
        workers[1].delay = 1;
        start(1, read_a);
        stopped(1);
        resume_io = 1;
        pthread_cond_broadcast(&event);
        while (!workers[0].done) wait_event();
        CHECK_EQ(sys_close(fd_a), 0);
        if (reuse) {
            CHECK_EQ(sys_pipe((int32_t *)buffer(2)), 0);
            int *fds = (int32_t *)buffer(2);
            CHECK_EQ(fds[0], fd_a);
            buffer(0)[0] = 'Z';
            CHECK_EQ(sys_write_file(fds[1], buffer(0), 1), 1);
        }
        workers[1].delay = 0;
        pthread_cond_broadcast(&event);
        finish(2);
        CHECK_EQ(workers[1].result, reuse ? 1 : -1);
        if (reuse) CHECK_EQ(buffer(1)[0], 'Z');
        CHECK_EQ(unlink_fs("/disk/a"), 0);
    }
}
static void test_wakes_and_errors(void) {
    TEST("spurious and kill wakes recheck ownership and return through cleanup");
    fresh();
    first(read_a, 0);
    start(1, read_a);
    stopped(1);
    int blocks = workers[1].blocks;
    workers[1].blocked = 0;
    workers[1].killed = 1;
    pthread_cond_broadcast(&event);
    while (workers[1].blocks == blocks && !workers[1].done) wait_event();
    CHECK(!workers[1].done);
    finish(2);
    CHECK(memcmp(buffer(1), seed + 7, 7) == 0);

    TEST("a buffer unmapped while waiting is revalidated before filesystem I/O");
    fresh();
    first(read_a, 0);
    start(1, read_a);
    stopped(1);
    CHECK_EQ(munmap(user_pages + USER_PAGE, USER_PAGE), 0);
    mapped[1] = 0;
    finish(2);
    CHECK_EQ(workers[1].result, -1);
    CHECK_EQ(sys_seek(fd_a, 0, SYS_SEEK_CUR), 7);
    CHECK(mmap(user_pages + USER_PAGE, USER_PAGE, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) ==
          user_pages + USER_PAGE);

    TEST("failed writes release the descriptor and preserve its offset");
    fresh();
    fail_write = 1;
    first(write_a, 1);
    start(1, close_a);
    stopped(1);
    finish(2);
    CHECK_EQ(workers[0].result, -1);
    CHECK_EQ(workers[1].result, 0);
    CHECK_EQ(sys_seek(fd_a, 0, SYS_SEEK_CUR), -1);
    CHECK_EQ(sys_close(fd_b), 0);
    CHECK(diskfs_format());

    TEST("invalid operations and zero writes release ownership without I/O");
    fresh();
    int before = write_calls;
    CHECK_EQ(sys_write_file(fd_a, NULL, 0), 0);
    CHECK_EQ(write_calls, before);
    CHECK_EQ(sys_seek(fd_a, -1, SYS_SEEK_SET), -1);
    CHECK_EQ(sys_seek(fd_a, 0, 123), -1);
    CHECK_EQ(sys_dup2(fd_a, fd_a), fd_a);
    CHECK_EQ(sys_close(fd_a), 0);
    CHECK_EQ(sys_close(fd_a), -1);
    CHECK_EQ(sys_read_file(fd_a, buffer(0), 1), -1);
    CHECK_EQ(sys_open(path("/disk/a")), fd_a);
    CHECK_EQ(sys_seek(fd_a, 0, SYS_SEEK_CUR), 0);
}
static int pipe_read_fd, pipe_write_fd;
static void pipe_wait_read(int id) {
    workers[id].result = sys_read_file(pipe_read_fd, buffer(id), 1);
}
static void pipe_wait_write(int id) {
    workers[id].result = sys_write_file(pipe_write_fd, buffer(id), 1);
}
static void replace_pipe(int id) {
    CHECK_EQ(sys_close(pipe_read_fd), 0);
    workers[id].result = sys_dup2(fd_a, pipe_write_fd);
}
static void test_independent_operations(void) {
    TEST("another descriptor on RAMFS progresses while DiskFS I/O is suspended");
    fresh();
    first(read_a, 0);
    start(1, ram_write);
    stopped(1);
    CHECK(workers[1].done);
    finish(2);
    CHECK_EQ(workers[1].result, 4);

    TEST("blocked pipe read permits close and dup2 to deliver EOF and free it");
    fresh();
    int baseline = live_allocs;
    CHECK_EQ(sys_pipe((int32_t *)buffer(0)), 0);
    pipe_read_fd = ((int32_t *)buffer(0))[0];
    pipe_write_fd = ((int32_t *)buffer(0))[1];
    start(0, pipe_wait_read);
    stopped(0);
    start(1, replace_pipe);
    finish(2);
    CHECK_EQ(workers[0].result, 0);
    CHECK_EQ(workers[1].result, pipe_write_fd);
    CHECK_EQ(live_allocs, baseline);
    CHECK_EQ(sys_close(pipe_write_fd), 0);

    TEST("blocked pipe write permits close/replacement to deliver broken pipe");
    fresh();
    baseline = live_allocs;
    CHECK_EQ(sys_pipe((int32_t *)buffer(0)), 0);
    pipe_read_fd = ((int32_t *)buffer(0))[0];
    pipe_write_fd = ((int32_t *)buffer(0))[1];
    memset(buffer(1), 'P', PIPE_BUF_SIZE);
    CHECK_EQ(sys_write_file(pipe_write_fd, buffer(1), PIPE_BUF_SIZE), PIPE_BUF_SIZE);
    start(0, pipe_wait_write);
    stopped(0);
    start(1, replace_pipe);
    finish(2);
    CHECK_EQ(workers[0].result, 0);
    CHECK_EQ(workers[1].result, pipe_write_fd);
    CHECK_EQ(live_allocs, baseline);
    CHECK_EQ(sys_close(pipe_write_fd), 0);
}

static void stdout_write(int id) { workers[id].result = sys_write(buffer(id), 7); }
static void stdin_read(int id) { workers[id].result = sys_read(buffer(id), 7); }
static void replace_stdout(int id) { workers[id].result = sys_dup2(fd_b, 1); }
static void replace_stdin(int id) { workers[id].result = sys_dup2(fd_b, 0); }
static void fork_streams(int id) {
    syscall_copy_user_files(&procs[0], &procs[1]);
    workers[id].result = 0;
}
static void test_stdio_offsets(void) {
    for (int reverse = 0; reverse < 2; reverse++) {
        TEST("stdout writes commit consecutive records in either worker order");
        fresh();
        CHECK_EQ(sys_dup2(fd_a, 1), 1);
        memset(buffer(0), reverse ? 'B' : 'A', 7);
        memset(buffer(1), reverse ? 'A' : 'B', 7);
        first(stdout_write, 1);
        start(1, stdout_write);
        stopped(1);
        finish(2);
        uint8_t expected[14];
        memcpy(expected, buffer(0), 7);
        memcpy(expected + 7, buffer(1), 7);
        check_bytes("a", expected, sizeof(expected));
        CHECK_EQ(workers[0].result, 7);
        CHECK_EQ(workers[1].result, 7);
        CHECK_EQ(sys_seek(fd_a, 0, SYS_SEEK_CUR), 0); /* independent dup offset */
    }
    TEST("stdin readers consume distinct consecutive bytes after device sleep");
    fresh();
    CHECK_EQ(sys_dup2(fd_a, 0), 0);
    first(stdin_read, 0);
    start(1, stdin_read);
    stopped(1);
    finish(2);
    CHECK(memcmp(buffer(0), seed, 7) == 0);
    CHECK(memcmp(buffer(1), seed + 7, 7) == 0);
    CHECK_EQ(sys_read(buffer(2), 7), 7);
    CHECK(memcmp(buffer(2), seed + 14, 7) == 0);
}
static void test_stdio_pio(void) {
    TEST("current kernel PIO preemption queues two stdout writers safely");
    fresh();
    CHECK_EQ(sys_dup2(fd_a, 1), 1);
    preempt = 1;
    workers[0].flags = IF_FLAG;
    first(kernel_write, 1);
    start(1, stdout_write);
    stopped(1);
    start(2, stdout_write);
    stopped(2);
    finish(3);
    uint8_t expected[14];
    memcpy(expected, buffer(1), 7);
    memcpy(expected + 7, buffer(2), 7);
    check_bytes("a", expected, sizeof(expected));
}
static void test_stdio_replacement(void) {
    for (int output = 0; output < 2; output++) {
        TEST("dup2 cannot apply completed standard-stream I/O to a new binding");
        fresh();
        CHECK_EQ(sys_dup2(fd_a, output), output);
        CHECK_EQ(sys_close(fd_a), 0); /* stream is the sole persistent owner */
        CHECK_EQ(sys_seek(fd_b, 30, SYS_SEEK_SET), 30);
        first(output ? stdout_write : stdin_read, output);
        start(1, output ? replace_stdout : replace_stdin);
        stopped(1);
        finish(2);
        CHECK_EQ(workers[1].result, output);
        CHECK_EQ(unlink_fs("/disk/a"), 0); /* old reference released exactly once */
        if (output) {
            CHECK_EQ(sys_write(buffer(2), 7), 7);
            uint8_t expected[37];
            memcpy(expected, seed, sizeof(expected));
            memcpy(expected + 30, buffer(2), 7);
            check_bytes("b", expected, sizeof(expected));
        } else {
            CHECK_EQ(sys_read(buffer(2), 7), 7);
            CHECK(memcmp(buffer(2), seed + 30, 7) == 0);
        }
        CHECK_EQ(sys_seek(fd_b, 0, SYS_SEEK_CUR), 30);
    }
}
static void test_stdio_fork(void) {
    for (int output = 0; output < 2; output++) {
        TEST("fork inherits committed stream offsets and independent references");
        fresh();
        CHECK_EQ(sys_dup2(fd_a, output), output);
        CHECK_EQ(sys_close(fd_a), 0);
        first(output ? stdout_write : stdin_read, output);
        start(1, fork_streams);
        stopped(1);
        finish(2);
        CHECK_EQ(workers[1].result, 0);
        controller_process = 1;
        close_streams(&procs[0]); /* cleanup while another process is current */
        syscall_close_user_files(&procs[0]);
        CHECK_EQ(unlink_fs("/disk/a"), -1);
        if (output) {
            CHECK_EQ(sys_write(buffer(2), 7), 7);
            uint8_t expected[14];
            memcpy(expected, buffer(0), 7);
            memcpy(expected + 7, buffer(2), 7);
            check_bytes("a", expected, sizeof(expected));
        } else {
            CHECK_EQ(sys_read(buffer(2), 7), 7);
            CHECK(memcmp(buffer(2), seed + 7, 7) == 0);
        }
        close_streams(&procs[1]);
        syscall_close_user_files(&procs[1]);
        CHECK_EQ(unlink_fs("/disk/a"), 0);
        controller_process = 0;
    }
    for (int output = 0; output < 2; output++) {
        TEST("fork retains each standard pipe end through parent cleanup");
        fresh();
        int baseline = live_allocs;
        CHECK_EQ(sys_pipe((int32_t *)buffer(2)), 0);
        int rfd = ((int32_t *)buffer(2))[0], wfd = ((int32_t *)buffer(2))[1];
        CHECK_EQ(sys_dup2(output ? wfd : rfd, output), output);
        CHECK_EQ(sys_close(output ? wfd : rfd), 0);
        syscall_copy_user_files(&procs[0], &procs[1]);
        controller_process = 1;
        close_streams(&procs[0]);
        syscall_close_user_files(&procs[0]);
        CHECK_EQ(output ? sys_write(buffer(0), 7) :
                 sys_write_file(wfd, buffer(0), 7), 7);
        CHECK_EQ(output ? sys_read_file(rfd, buffer(1), 7) :
                 sys_read(buffer(1), 7), 7);
        CHECK(memcmp(buffer(0), buffer(1), 7) == 0);
        close_streams(&procs[1]);
        syscall_close_user_files(&procs[1]);
        CHECK_EQ(live_allocs, baseline);
        controller_process = 0;
    }
}
static void test_stdio_waits(void) {
    TEST("stdin gate rechecks spurious and kill wakes before consuming bytes");
    fresh();
    CHECK_EQ(sys_dup2(fd_a, 0), 0);
    first(stdin_read, 0);
    start(1, stdin_read);
    stopped(1);
    int blocks = workers[1].blocks;
    workers[1].killed = 1;
    workers[1].blocked = 0;
    pthread_cond_broadcast(&event);
    while (workers[1].blocks == blocks && !workers[1].done) wait_event();
    CHECK(!workers[1].done);
    finish(2);
    CHECK(memcmp(buffer(1), seed + 7, 7) == 0);

    for (int output = 0; output < 2; output++) {
        TEST("standard I/O rejects a buffer unmapped during stream-gate waiting");
        fresh();
        CHECK_EQ(sys_dup2(fd_a, output), output);
        first(output ? stdout_write : stdin_read, output);
        start(1, output ? stdout_write : stdin_read);
        stopped(1);
        CHECK_EQ(munmap(user_pages + USER_PAGE, USER_PAGE), 0);
        mapped[1] = 0;
        finish(2);
        CHECK_EQ(workers[1].result, -1);
        CHECK(mmap(user_pages + USER_PAGE, USER_PAGE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) ==
              user_pages + USER_PAGE);
        mapped[1] = 1;
        /* A new valid call succeeds, proving the rejected call released. */
        CHECK_EQ(output ? sys_write(buffer(2), 1) : sys_read(buffer(2), 1), 1);
        if (!output) CHECK_EQ((uint8_t)buffer(2)[0], seed[7]);
    }

    TEST("failed stdout I/O releases ownership for redirection and preserves ABI");
    fresh();
    CHECK_EQ(sys_dup2(fd_a, 1), 1);
    CHECK_EQ(sys_close(fd_b), 0);
    fd_b = sys_open(path("/ram"));
    CHECK(fd_b >= 0);
    fail_write = 1;
    first(stdout_write, 1);
    start(1, replace_stdout);
    stopped(1);
    finish(2);
    CHECK_EQ(workers[0].result, 0); /* SYS_WRITE historically returns zero here */
    CHECK_EQ(workers[1].result, 1);
    CHECK_EQ(sys_write(buffer(2), 7), 7);
    CHECK_EQ(sys_read_file(fd_b, buffer(1), 7), 7);
    CHECK(memcmp(buffer(1), buffer(2), 7) == 0);
}
static void test_stdio_lookup(void) {
    TEST("waiting dup2 samples a reused source only after owning the stream");
    fresh();
    CHECK_EQ(sys_dup2(fd_a, 1), 1);
    first(stdout_write, 1);
    start(1, replace_stdout);
    stopped(1);
    /* dup2 must not hold its source while waiting on a lower-numbered stream.
     * The source can change; lookup must then use its new node AND offset. */
    CHECK_EQ(sys_close(fd_b), 0);
    CHECK_EQ(sys_open(path("/ram")), fd_b);
    CHECK_EQ(sys_seek(fd_b, 3, SYS_SEEK_SET), 3);
    finish(2);
    CHECK_EQ(sys_write(buffer(2), 7), 7);
    CHECK_EQ(sys_read_file(fd_b, buffer(1), 7), 7);
    CHECK(memcmp(buffer(1), buffer(2), 7) == 0);

    TEST("queued stdin read resolves a replacement pipe after acquiring stream");
    fresh();
    CHECK_EQ(sys_dup2(fd_a, 0), 0);
    first(stdin_read, 0);
    workers[1].delay = 1;
    start(1, stdin_read);
    stopped(1);
    resume_io = 1;
    pthread_cond_broadcast(&event);
    while (!workers[0].done) wait_event();
    CHECK_EQ(sys_pipe((int32_t *)buffer(2)), 0);
    int rfd = ((int32_t *)buffer(2))[0], wfd = ((int32_t *)buffer(2))[1];
    CHECK_EQ(sys_dup2(rfd, 0), 0);
    buffer(0)[0] = 'Z';
    CHECK_EQ(sys_write_file(wfd, buffer(0), 1), 1);
    workers[1].delay = 0;
    pthread_cond_broadcast(&event);
    finish(2);
    CHECK_EQ(workers[1].result, 1);
    CHECK_EQ(buffer(1)[0], 'Z');
}
static void test_stdio_devices(void) {
    TEST("blocked stdout does not serialize independent stdin or another process");
    fresh();
    CHECK_EQ(sys_dup2(fd_a, 1), 1);
    int ram = sys_open(path("/ram"));
    CHECK_EQ(sys_dup2(ram, 0), 0);
    first(stdout_write, 1);
    CHECK_EQ(sys_read(buffer(1), 7), 7);
    CHECK(memcmp(buffer(1), seed + 17, 7) == 0);
    controller_process = 1;
    CHECK_EQ(sys_write(buffer(2), 7), 7);
    CHECK_EQ(terminal_bytes, 7);
    controller_process = 0;
    finish(1);

    for (int output = 0; output < 2; output++) {
        TEST("blocked standard pipe releases stream so replacement and EOF progress");
        fresh();
        int baseline = live_allocs;
        CHECK_EQ(sys_pipe((int32_t *)buffer(2)), 0);
        int rfd = ((int32_t *)buffer(2))[0], wfd = ((int32_t *)buffer(2))[1];
        CHECK_EQ(sys_dup2(output ? wfd : rfd, output), output);
        if (output) {
            /* PIPE_BUF_SIZE spans two fixture pages; leave one after it. */
            memset(buffer(1), 'P', PIPE_BUF_SIZE);
            CHECK_EQ(sys_write_file(wfd, buffer(1), PIPE_BUF_SIZE), PIPE_BUF_SIZE);
        }
        start(0, output ? stdout_write : stdin_read);
        stopped(0);
        CHECK(!workers[0].done);
        CHECK_EQ(sys_dup2(fd_b, output), output);
        CHECK_EQ(sys_close(rfd), 0);
        CHECK_EQ(sys_close(wfd), 0);
        finish(1);
        CHECK_EQ(workers[0].result, 0);
        CHECK_EQ(live_allocs, baseline);
        CHECK_EQ(output ? sys_write(buffer(1), 7) : sys_read(buffer(1), 7), 7);
        if (!output) CHECK(memcmp(buffer(1), seed, 7) == 0);
    }

    TEST("keyboard wait releases stdin so a sibling can redirect it");
    fresh();
    keyboard_wait = 1;
    start(0, stdin_read);
    stopped(0);
    CHECK_EQ(sys_dup2(fd_b, 0), 0);
    task_wake_all(&keyboard_wait);
    finish(1);
    CHECK_EQ(workers[0].result, 1);
    CHECK_EQ(buffer(0)[0], 'K');
    CHECK_EQ(sys_read(buffer(1), 7), 7);
    CHECK(memcmp(buffer(1), seed, 7) == 0);
    CHECK_EQ(keyboard_calls, 1);

    TEST("zero and invalid stream operations leave locks and bindings usable");
    fresh();
    CHECK_EQ(sys_dup2(fd_a, 0), 0);
    CHECK_EQ(sys_dup2(fd_a, 1), 1);
    CHECK_EQ(sys_read(NULL, 0), 0);
    CHECK_EQ(sys_write(NULL, 0), 0);
    CHECK_EQ(write_calls, 0);
    CHECK_EQ(sys_pipe((int32_t *)buffer(2)), 0);
    int rfd = ((int32_t *)buffer(2))[0], wfd = ((int32_t *)buffer(2))[1];
    CHECK_EQ(sys_dup2(rfd, 1), -1);
    CHECK_EQ(sys_dup2(wfd, 0), -1);
    CHECK_EQ(sys_read(buffer(0), 1), 1);
    CHECK_EQ((uint8_t)buffer(0)[0], seed[0]);
    CHECK_EQ(sys_write(buffer(1), 1), 1);
}
int main(int argc, char **argv) {
    void *want = (void *)(uintptr_t)USER_EXT_BASE;
    user_pages = mmap(want, WORKERS * USER_PAGE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (user_pages != want) { CHECK(0 && "map real syscall user buffers"); return 1; }
    for (unsigned i = 0; i < sizeof(seed); i++) seed[i] = (uint8_t)(i * 13 + 7);
    pthread_mutex_lock(&cpu);
    ramfs_init();
    pause_actor = -2;
    diskfs_install();
    CHECK(diskfs_format());
    CHECK_EQ(ramfs_mount("/disk", diskfs_get_root_node()), 0);
    CHECK(ramfs_create_file("/ram") != NULL);
    struct { const char *name; void (*run)(void); } cases[] = {
        {"offsets", test_shared_offsets}, {"pio", test_current_pio_boundary},
        {"observers", test_offset_observers}, {"replacement", test_replacement},
        {"reservation", test_reserved_destination}, {"lookup", test_queued_lookup},
        {"errors", test_wakes_and_errors}, {"independent", test_independent_operations},
        {"stdio-offsets", test_stdio_offsets}, {"stdio-pio", test_stdio_pio},
        {"stdio-replacement", test_stdio_replacement}, {"stdio-fork", test_stdio_fork},
        {"stdio-waits", test_stdio_waits}, {"stdio-lookup", test_stdio_lookup},
        {"stdio-devices", test_stdio_devices}
    };
    int selected = 0;
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (argc > 1 && strcmp(argv[1], cases[i].name) != 0) continue;
        cases[i].run();
        selected++;
    }
    CHECK(selected != 0);
    close_streams(&procs[0]);
    close_streams(&procs[1]);
    syscall_close_user_files(&procs[0]);
    syscall_close_user_files(&procs[1]);
    CHECK(diskfs_format());
    CHECK_EQ(unlink_fs("/ram"), 0);
    CHECK_EQ(munmap(user_pages, WORKERS * USER_PAGE), 0);
    pthread_mutex_unlock(&cpu);
    TEST_REPORT("fd-operations");
}
