#include "user_syscall.h"
#include "umalloc.h"

/*
 * Integrated kernel stress test.
 *
 * This deliberately runs in ring 3.  Native tests can drive individual state
 * machines much harder, but only a real user process exercises page faults,
 * interrupt-driven preemption, the assembly context switch, syscall pointer
 * validation, and cross-subsystem teardown together.
 */

#define ARRAY_SIZE(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define PAGE_SIZE 4096

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

static int bytes_equal(const unsigned char *a, const unsigned char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

static int fail(const char *name) {
    write_str("[stress ");
    write_str(name);
    write_str(" FAIL]\n");
    return -1;
}

static void pass(const char *name) {
    write_str("[stress ");
    write_str(name);
    write_str(" ok]\n");
}

static int unknown_syscall(void) {
    int ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(0x7FFFFFFF)
                     : "memory");
    return ret;
}

static int test_invalid_pointers(void) {
    const char *bad = (const char *)0x1000;
    struct ustat st;

    if (sys_write(bad, 1) != -1 ||
        sys_write((const char *)0x3EFFFF, 2) != -1 ||
        sys_open(bad) != -1 ||
        sys_create(bad) != -1 ||
        sys_stat("readme.txt", (struct ustat *)bad) != -1 ||
        sys_getcwd((char *)bad, 8) != -1 ||
        sys_getprocs((struct uproc *)bad, 1) != -1 ||
        sys_time((struct utime *)bad) != -1 ||
        sys_pipe((int *)bad) != -1 ||
        sys_readdir("/", 0, (char *)bad) != -1 ||
        sys_waitpid(-1, (int *)bad, WNOHANG) != -1 ||
        sys_getenv("missing", (char *)bad, 8) != -1 ||
        unknown_syscall() != -1) {
        return fail("invalid pointers");
    }

    /* A valid pointer must still work after the rejection barrage. */
    if (sys_stat("readme.txt", &st) != 0 || !S_ISREG(st.type))
        return fail("invalid pointer recovery");

    pass("invalid pointers");
    return 0;
}

static int test_fault_isolation(void) {
    static const char *modes[] = {
        "page", "divide", "invalid", "privileged"
    };
    const int rounds = 24;

    for (int i = 0; i < rounds; i++) {
        const char *fault_argv[] = { "fault", modes[i % ARRAY_SIZE(modes)] };
        int status = 0;
        int pid = sys_spawn_argv(ARRAY_SIZE(fault_argv), fault_argv);

        if (pid < 0) return fail("fault isolation spawn");
        if (sys_waitpid(pid, &status, 0) != pid)
            return fail("fault isolation reap");
        if (status != -1) return fail("fault isolation status");
    }

    write_str("[stress fault isolation iterations=");
    write_int(rounds);
    write_str("]\n");
    pass("fault isolation");
    return 0;
}

static void *mmap_slots[1024];

#define HEAP_EXHAUST_SLOTS 256
#define HEAP_EXHAUST_BYTES 4000
static unsigned char *heap_exhaust_slots[HEAP_EXHAUST_SLOTS];

static int test_heap_and_paging(void) {
    unsigned char *blocks[32];
    unsigned sizes[32];
    void *first_base = 0;

    /* Fragment, validate, and coalesce the user allocator repeatedly. */
    for (int round = 0; round < 24; round++) {
        for (int i = 0; i < ARRAY_SIZE(blocks); i++) {
            sizes[i] = 31u + (unsigned)((round * 37 + i * 97) % 1800);
            blocks[i] = (unsigned char *)malloc(sizes[i]);
            if (!blocks[i]) return fail("heap allocation");
            for (unsigned j = 0; j < sizes[i]; j++)
                blocks[i][j] = (unsigned char)(round ^ i ^ (int)j);
        }
        for (int i = 0; i < ARRAY_SIZE(blocks); i++) {
            for (unsigned j = 0; j < sizes[i]; j++) {
                if (blocks[i][j] != (unsigned char)(round ^ i ^ (int)j))
                    return fail("heap contents");
            }
        }
        for (int i = 0; i < ARRAY_SIZE(blocks); i += 2) free(blocks[i]);
        for (int i = 1; i < ARRAY_SIZE(blocks); i += 2) free(blocks[i]);
    }

    /* Force demand faults across a sizeable sbrk-backed range. */
    {
        unsigned char *big = (unsigned char *)malloc(256 * 1024);
        if (!big) return fail("heap demand paging");
        for (int i = 0; i < 256 * 1024; i += PAGE_SIZE)
            big[i] = (unsigned char)(i / PAGE_SIZE + 3);
        for (int i = 0; i < 256 * 1024; i += PAGE_SIZE) {
            if (big[i] != (unsigned char)(i / PAGE_SIZE + 3))
                return fail("heap page contents");
        }
        free(big);
    }

    /* Drive the sbrk-backed allocator to a real failure, not merely to a
     * fixed iteration count.  The slot bound is deliberately larger than the
     * entire user heap: reaching it would mean this test never proved the
     * exhaustion path.  Touch both ends of every allocation so demand paging
     * and allocator metadata remain coupled to the check. */
    {
        int count = 0;

        while (count < HEAP_EXHAUST_SLOTS) {
            unsigned char *p = (unsigned char *)malloc(HEAP_EXHAUST_BYTES);
            if (!p) break;
            p[0] = (unsigned char)(count * 19 + 7);
            p[HEAP_EXHAUST_BYTES - 1] = (unsigned char)(count * 23 + 11);
            heap_exhaust_slots[count++] = p;
        }
        if (count < 128 || count == HEAP_EXHAUST_SLOTS)
            return fail("heap exhaustion capacity");
        {
            void *unexpected = malloc(HEAP_EXHAUST_BYTES);
            if (unexpected) {
                free(unexpected);
                return fail("heap exhaustion limit");
            }
        }

        write_str("[stress heap exhaustion allocations=");
        write_int(count);
        write_str("]\n");

        for (int i = 0; i < count; i++) {
            if (heap_exhaust_slots[i][0] != (unsigned char)(i * 19 + 7) ||
                heap_exhaust_slots[i][HEAP_EXHAUST_BYTES - 1] !=
                    (unsigned char)(i * 23 + 11)) {
                return fail("heap exhaustion contents");
            }
        }
        for (int i = count - 1; i >= 0; i--) free(heap_exhaust_slots[i]);

        /* Reverse-order frees must coalesce the contiguous sbrk arena well
         * enough to serve a request much larger than an individual chunk. */
        {
            unsigned char *again = (unsigned char *)malloc(128 * 1024);
            if (!again) return fail("heap exhaustion recovery");
            for (int i = 0; i < 128 * 1024; i += PAGE_SIZE)
                again[i] = (unsigned char)(i / PAGE_SIZE + 29);
            for (int i = 0; i < 128 * 1024; i += PAGE_SIZE) {
                if (again[i] != (unsigned char)(i / PAGE_SIZE + 29))
                    return fail("heap recovery contents");
            }
            free(again);
        }
    }
    pass("heap exhaustion");

    /* Repeated page-in/unmap cycles must reuse the same virtual run. */
    for (int round = 0; round < 32; round++) {
        unsigned char *p = (unsigned char *)sys_mmap(16);
        if (!p || (first_base && p != first_base))
            return fail("mmap reuse");
        if (!first_base) first_base = p;
        for (int i = 0; i < 16; i++) p[i * PAGE_SIZE] = (unsigned char)(round + i);
        for (int i = 0; i < 16; i++) {
            if (p[i * PAGE_SIZE] != (unsigned char)(round + i))
                return fail("mmap contents");
        }
        if (sys_munmap(p, 16) != 0 || sys_munmap(p, 16) != -1)
            return fail("munmap validation");
    }

    /* Reserve every page in the 4 MB mmap region without touching it. */
    for (int i = 0; i < ARRAY_SIZE(mmap_slots); i++) {
        mmap_slots[i] = sys_mmap(1);
        if (mmap_slots[i] != (void *)(0x2000000u + (unsigned)i * PAGE_SIZE))
            return fail("mmap exhaustion fill");
    }
    if (sys_mmap(1) != 0) return fail("mmap exhaustion limit");
    for (int i = ARRAY_SIZE(mmap_slots) - 1; i >= 0; i--) {
        if (sys_munmap(mmap_slots[i], 1) != 0)
            return fail("mmap exhaustion cleanup");
    }

    /* Allocate the whole region as one run and fault in every physical page. */
    {
        unsigned char *whole = (unsigned char *)sys_mmap(1024);
        if (whole != (unsigned char *)0x2000000u)
            return fail("mmap whole region");
        for (int i = 0; i < 1024; i++)
            whole[i * PAGE_SIZE] = (unsigned char)(i * 13 + 5);
        for (int i = 0; i < 1024; i++) {
            if (whole[i * PAGE_SIZE] != (unsigned char)(i * 13 + 5))
                return fail("physical page contents");
        }
        if (sys_munmap(whole, 1024) != 0)
            return fail("physical page cleanup");
    }

    pass("heap and paging");
    return 0;
}

static int test_descriptor_and_pipe_exhaustion(void) {
    int fds[8];
    int pipes[4][2];

    for (int i = 0; i < ARRAY_SIZE(fds); i++) {
        fds[i] = sys_open("readme.txt");
        if (fds[i] != i + 3) return fail("fd exhaustion fill");
    }
    if (sys_open("readme.txt") != -1) return fail("fd exhaustion limit");
    for (int i = ARRAY_SIZE(fds) - 1; i >= 0; i--) {
        if (sys_close(fds[i]) != 0) return fail("fd exhaustion cleanup");
    }
    fds[0] = sys_open("readme.txt");
    if (fds[0] != 3 || sys_close(fds[0]) != 0)
        return fail("fd exhaustion recovery");

    for (int round = 0; round < 12; round++) {
        for (int i = 0; i < ARRAY_SIZE(pipes); i++) {
            if (sys_pipe(pipes[i]) != 0) return fail("pipe exhaustion fill");
        }
        {
            int extra[2];
            if (sys_pipe(extra) != -1) return fail("pipe exhaustion limit");
        }
        for (int i = ARRAY_SIZE(pipes) - 1; i >= 0; i--) {
            if (sys_close(pipes[i][1]) != 0 || sys_close(pipes[i][0]) != 0)
                return fail("pipe exhaustion cleanup");
        }
    }

    pass("fd and pipe exhaustion");
    return 0;
}

static int file_cycle(const char *path, int seed) {
    unsigned char payload[96];
    unsigned char readback[96];
    struct ustat st;
    int fd;

    for (int i = 0; i < ARRAY_SIZE(payload); i++)
        payload[i] = (unsigned char)(seed * 17 + i * 11);

    fd = sys_create(path);
    if (fd < 0 ||
        sys_write_file(fd, (const char *)payload, sizeof(payload)) != sizeof(payload) ||
        sys_seek(fd, 0, SEEK_SET) != 0 ||
        sys_read_file(fd, (char *)readback, sizeof(readback)) != sizeof(readback) ||
        !bytes_equal(payload, readback, sizeof(payload)) ||
        sys_fstat(fd, &st) != 0 || st.size != sizeof(payload) ||
        sys_unlink(path) != -1 ||
        sys_close(fd) != 0) {
        return -1;
    }

    fd = sys_open(path);
    if (fd < 0 ||
        sys_read_file(fd, (char *)readback, sizeof(readback)) != sizeof(readback) ||
        !bytes_equal(payload, readback, sizeof(payload)) ||
        sys_close(fd) != 0 ||
        sys_unlink(path) != 0 ||
        sys_open(path) != -1) {
        return -1;
    }
    return 0;
}

static const char *ram_fill_paths[] = {
    "rx0", "rx1", "rx2", "rx3", "rx4", "rx5", "rx6", "rx7"
};

static const char *disk_fill_paths[] = {
    "/disk/d00", "/disk/d01", "/disk/d02", "/disk/d03",
    "/disk/d04", "/disk/d05", "/disk/d06", "/disk/d07",
    "/disk/d08", "/disk/d09", "/disk/d10", "/disk/d11",
    "/disk/d12", "/disk/d13", "/disk/d14", "/disk/d15"
};

static int test_filesystems(void) {
    static const char *files[] = {
        "stress.tmp", "/disk/stress.tmp", "/fat/stress.tmp"
    };
    static const char *dirs[] = { "stressdir", "/disk/stressdir" };

    for (int round = 0; round < 12; round++) {
        for (int i = 0; i < ARRAY_SIZE(files); i++) {
            if (file_cycle(files[i], round * ARRAY_SIZE(files) + i) != 0)
                return fail("filesystem file cycle");
        }
        for (int i = 0; i < ARRAY_SIZE(dirs); i++) {
            if (sys_mkdir(dirs[i]) != 0 || sys_rmdir(dirs[i]) != 0)
                return fail("filesystem directory cycle");
        }
    }

    /* Fill the remaining RAMFS slots, then prove cleanup restores capacity. */
    {
        int opened[ARRAY_SIZE(ram_fill_paths)];
        int count = 0;
        while (count < ARRAY_SIZE(ram_fill_paths)) {
            int fd = sys_create(ram_fill_paths[count]);
            if (fd < 0) break;
            opened[count++] = fd;
        }
        if (count == 0 || count == ARRAY_SIZE(ram_fill_paths))
            return fail("ramfs exhaustion limit");
        for (int i = 0; i < count; i++) {
            if (sys_close(opened[i]) != 0 || sys_unlink(ram_fill_paths[i]) != 0)
                return fail("ramfs exhaustion cleanup");
        }
        {
            int fd = sys_create("rxr");
            if (fd < 0 || sys_close(fd) != 0 || sys_unlink("rxr") != 0)
                return fail("ramfs exhaustion recovery");
        }
    }

    /* DiskFS has 16 slots; seed.txt owns one, so exactly 15 must be available. */
    {
        int count = 0;
        for (; count < ARRAY_SIZE(disk_fill_paths); count++) {
            int fd = sys_create(disk_fill_paths[count]);
            if (fd < 0) break;
            if (sys_close(fd) != 0) return fail("diskfs exhaustion close");
        }
        if (count != 15) return fail("diskfs exhaustion limit");
        for (int i = 0; i < count; i++) {
            if (sys_unlink(disk_fill_paths[i]) != 0)
                return fail("diskfs exhaustion cleanup");
        }
        {
            int fd = sys_create("/disk/recover");
            if (fd < 0 || sys_close(fd) != 0 ||
                sys_unlink("/disk/recover") != 0) {
                return fail("diskfs exhaustion recovery");
            }
        }
    }

    pass("filesystems");
    return 0;
}

static volatile int alarm_seen;

static void alarm_handler(int signum) {
    (void)signum;
    alarm_seen = 1;
}

static int test_interrupts_and_preemption(void) {
    volatile unsigned *shared;
    int children[3];
    int start;

    alarm_seen = 0;
    if (sys_signal(SIGALRM, alarm_handler) != 0 || sys_alarm(3) < 0)
        return fail("timer alarm setup");
    start = sys_uptime();
    while (!alarm_seen && (unsigned)(sys_uptime() - start) < 100u) {
        __asm__ volatile("" ::: "memory");
    }
    if (!alarm_seen) return fail("timer alarm delivery");

    shared = (volatile unsigned *)sys_shm();
    if (!shared) return fail("preemption shared page");
    for (int i = 0; i < 4; i++) shared[i] = 0;

    for (int i = 0; i < ARRAY_SIZE(children); i++) {
        int pid = sys_fork();
        if (pid < 0) return fail("preemption fork");
        if (pid == 0) {
            volatile unsigned value = 0x1234567u + (unsigned)i;
            for (int spin = 0; spin < 250000; spin++)
                value = value * 1664525u + 1013904223u;
            shared[i] = value | 1u;
            sys_exit(20 + i);
        }
        children[i] = pid;
    }

    /* No yield/sleep here: only the PIT scheduler can let the children run. */
    start = sys_uptime();
    while ((!shared[0] || !shared[1] || !shared[2]) &&
           (unsigned)(sys_uptime() - start) < 150u) {
        __asm__ volatile("" ::: "memory");
    }
    if (!shared[0] || !shared[1] || !shared[2])
        return fail("timer preemption");

    for (int i = 0; i < ARRAY_SIZE(children); i++) {
        int status = 0;
        if (sys_waitpid(children[i], &status, 0) != children[i] || status != 20 + i)
            return fail("preemption reap");
    }

    pass("interrupts and preemption");
    return 0;
}

#define THREAD_SEM 7
#define THREAD_STACK_PAGES 4
#define THREAD_ROUNDS 10
#define THREAD_ITERS 64

static volatile int thread_counter;

static void thread_worker(void) {
    volatile unsigned canary = 0xC0FFEE11u;

    for (int i = 0; i < THREAD_ITERS; i++) {
        if (canary != 0xC0FFEE11u) sys_exit(91);
        if (sys_sem_wait(THREAD_SEM) != 0) sys_exit(92);
        thread_counter++;
        if (sys_sem_post(THREAD_SEM) != 0) sys_exit(93);
        sys_yield();
    }
    sys_exit(0);
}

static int test_threads_and_context_switches(void) {
    for (int round = 0; round < THREAD_ROUNDS; round++) {
        char *stack1 = (char *)sys_mmap(THREAD_STACK_PAGES);
        char *stack2 = (char *)sys_mmap(THREAD_STACK_PAGES);

        if (!stack1 || !stack2) return fail("thread stack allocation");
        thread_counter = 0;
        if (sys_sem_init(THREAD_SEM, 1) != 0 ||
            sys_thread_create(thread_worker,
                              stack1 + THREAD_STACK_PAGES * PAGE_SIZE) < 0 ||
            sys_thread_create(thread_worker,
                              stack2 + THREAD_STACK_PAGES * PAGE_SIZE) < 0) {
            return fail("thread creation");
        }
        sys_thread_join();
        if (thread_counter != 2 * THREAD_ITERS)
            return fail("thread shared state");
        if (sys_munmap(stack1, THREAD_STACK_PAGES) != 0 ||
            sys_munmap(stack2, THREAD_STACK_PAGES) != 0) {
            return fail("thread stack cleanup");
        }
        sys_yield();
    }

    pass("scheduling and context switches");
    return 0;
}

static int test_process_exhaustion(void) {
    int children[15];

    for (int i = 0; i < ARRAY_SIZE(children); i++) {
        int pid = sys_fork();
        if (pid < 0) return fail("process exhaustion fill");
        if (pid == 0) sys_exit(i + 1);
        children[i] = pid;
    }
    if (sys_fork() != -1) return fail("process exhaustion limit");

    {
        struct uproc procs[16];
        if (sys_getprocs(procs, ARRAY_SIZE(procs)) != 16)
            return fail("process table snapshot");
    }

    for (int i = 0; i < ARRAY_SIZE(children); i++) {
        int status = 0;
        if (sys_waitpid(children[i], &status, 0) != children[i] || status != i + 1)
            return fail("process exhaustion cleanup");
    }
    if (sys_waitpid(-1, 0, WNOHANG) != -1)
        return fail("process exhaustion empty");

    {
        int status = 0;
        int pid = sys_fork();
        if (pid < 0) return fail("process exhaustion recovery");
        if (pid == 0) sys_exit(77);
        if (sys_waitpid(pid, &status, 0) != pid || status != 77)
            return fail("process recovery reap");
    }

    pass("process exhaustion");
    return 0;
}

static volatile unsigned cow_data[1024];

static int test_repeated_lifecycle(void) {
    const char *hello_argv[] = { "hello" };

    for (int i = 0; i < ARRAY_SIZE(cow_data); i++)
        cow_data[i] = 0xA5000000u ^ (unsigned)i;

    for (int round = 0; round < 48; round++) {
        int pid = sys_fork();
        if (pid < 0) return fail("fork lifecycle create");
        if (pid == 0) {
            volatile unsigned guard[4] = {
                0x11223344u, 0x55667788u, 0x99AABBCCu, 0xDDEEFF00u
            };
            unsigned state = 0x31415926u ^ (unsigned)round;

            cow_data[round % ARRAY_SIZE(cow_data)] ^= 0xFFFFFFFFu;
            for (int i = 0; i < 96; i++) {
                state = state * 1103515245u + 12345u;
                sys_yield();
                if (guard[0] != 0x11223344u || guard[1] != 0x55667788u ||
                    guard[2] != 0x99AABBCCu || guard[3] != 0xDDEEFF00u) {
                    sys_exit(81);
                }
            }
            sys_exit(state == 0 ? 82 : 0);
        }

        {
            int status = 0;
            if (sys_waitpid(pid, &status, 0) != pid || status != 0)
                return fail("fork lifecycle reap");
        }
        for (int i = 0; i < ARRAY_SIZE(cow_data); i++) {
            if (cow_data[i] != (0xA5000000u ^ (unsigned)i))
                return fail("fork cow isolation");
        }
    }

    /* Repeated ELF load + process launch/teardown through the syscall path. */
    for (int i = 0; i < 8; i++) {
        if (sys_exec_argv(1, hello_argv) != 0)
            return fail("exec lifecycle");
    }

    pass("repeated lifecycle");
    return 0;
}

int main(void) {
    write_str("[stress BEGIN]\n");

    if (test_invalid_pointers() != 0 ||
        test_fault_isolation() != 0 ||
        test_heap_and_paging() != 0 ||
        test_descriptor_and_pipe_exhaustion() != 0 ||
        test_filesystems() != 0 ||
        test_interrupts_and_preemption() != 0 ||
        test_threads_and_context_switches() != 0 ||
        test_process_exhaustion() != 0 ||
        test_repeated_lifecycle() != 0) {
        write_str("[stress FAILED]\n");
        return 1;
    }

    write_str("[stress PASS]\n");
    return 0;
}
