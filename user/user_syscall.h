#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* Inline syscall wrappers for user-space programs running on miniOS.
 * Convention: int $0x80, eax=number, ebx=arg1, ecx=arg2, edx=arg3 */

static inline int sys_write(const char *buf, int len) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(1), "b"(buf), "c"(len)
        : "memory"
    );
    return ret;
}

static inline int sys_read(char *buf, int len) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(2), "b"(buf), "c"(len)
        : "memory"
    );
    return ret;
}

static inline int sys_open(const char *name) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(5), "b"(name)
        : "memory"
    );
    return ret;
}

static inline int sys_read_file(int fd, char *buf, int len) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(6), "b"(fd), "c"(buf), "d"(len)
        : "memory"
    );
    return ret;
}

static inline int sys_close(int fd) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(7), "b"(fd)
        : "memory"
    );
    return ret;
}

/* Clone the current process (SYS_FORK = 26). Returns the child pid in the
 * parent and 0 in the child, or -1 on failure. */
static inline int sys_fork(void) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(26)
        : "memory"
    );
    return ret;
}

/* Replace the current process image with a new program (SYS_EXECV = 27).
 * Does not return on success; returns -1 on failure. */
static inline int sys_execv(int argc, const char **argv) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(27), "b"(argv), "c"(argc)
        : "memory"
    );
    return ret;
}

static inline int sys_spawn(const char *name) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(8), "b"(name)
        : "memory"
    );
    return ret;
}

static inline int sys_wait(int pid) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(9), "b"(pid)
        : "memory"
    );
    return ret;
}

#define WNOHANG 1
#define WEXITSTATUS(s) ((s) & 0xFF)

/* Reap a child (SYS_WAITPID = 28). pid > 0 waits for that child, pid == -1 for
 * any child. Writes the exit status through `status`. Returns the child pid, 0
 * if WNOHANG and none ready, or -1 if there is no such child. */
static inline int sys_waitpid(int pid, int *status, int options) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(28), "b"(pid), "c"(status), "d"(options)
        : "memory"
    );
    return ret;
}

static inline void sys_yield(void) {
    __asm__ volatile(
        "int $0x80"
        :: "a"(10)
        : "memory"
    );
}

static inline int sys_getpid(void) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(11)
        : "memory"
    );
    return ret;
}

/* Parent process id (SYS_GETPPID = 35). */
static inline int sys_getppid(void) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(35)
        : "memory"
    );
    return ret;
}

/* One process-table entry (matches the kernel process_info_t layout). */
struct uproc {
    int pid;
    int ppid;
    int state;        /* 1 = running, 2 = zombie */
    char name[16];
};

/* Snapshot the process table (SYS_GETPROCS = 38). Returns the entry count. */
static inline int sys_getprocs(struct uproc *buf, int max) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(38), "b"(buf), "c"(max)
        : "memory"
    );
    return ret;
}

/* Timer ticks since boot (SYS_UPTIME = 39); the PIT runs at 100 Hz. */
static inline int sys_uptime(void) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(39)
        : "memory"
    );
    return ret;
}

/* Wall-clock time from the CMOS RTC (SYS_TIME = 40). Mirrors kernel rtc_time_t. */
struct utime {
    unsigned short year;
    unsigned char month;
    unsigned char day;
    unsigned char hour;
    unsigned char minute;
    unsigned char second;
};

static inline int sys_time(struct utime *buf) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(40), "b"(buf)
        : "memory"
    );
    return ret;
}

/* Spawn a thread running `entry` on `stack_top`, sharing this process's whole
 * address space (SYS_THREAD_CREATE = 49). Returns a thread id, or -1. The
 * thread must finish by calling sys_exit(). The process (and its address
 * space) only becomes reapable once every thread -- main included -- has
 * exited: if main exits first, the kernel defers the actual teardown until
 * the last thread finishes, so a parent's wait()/waitpid() on this process
 * blocks for the full lifetime. Prefer sys_thread_join() before exiting
 * anyway when you want a deterministic point at which all threads are known
 * to be done (see threadexit.c for a case that relies on the deferred path). */
static inline int sys_thread_create(void (*entry)(void), void *stack_top) {
    int ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(49), "b"(entry), "c"(stack_top)
                     : "memory");
    return ret;
}

/* Block until every thread of this process has exited (SYS_THREAD_JOIN = 50). */
static inline void sys_thread_join(void) {
    __asm__ volatile("int $0x80" : : "a"(50) : "memory");
}

/* Reserve `npages` of demand-paged memory in the mmap region (SYS_MMAP = 48),
 * returning the base address (or NULL). This region lives above the old 4MB
 * low window, so large allocations no longer have to fit beside the stack. */
static inline void *sys_mmap(int npages) {
    void *ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(48), "b"(npages) : "memory");
    return ret;
}

/* Release `npages` starting at `addr` back to the mmap region (SYS_MUNMAP = 51).
 * The physical memory is freed and a later sys_mmap may hand the same addresses
 * out again; touching the pages after munmap kills the program. Every page in
 * the range must have come from sys_mmap and still be mapped. Returns 0 or -1. */
static inline int sys_munmap(void *addr, int npages) {
    int ret;
    __asm__ volatile("int $0x80"
                     : "=a"(ret)
                     : "a"(51), "b"(addr), "c"(npages)
                     : "memory");
    return ret;
}

/* Map a shared-memory page (SYS_SHM = 44), returning its address (or NULL).
 * Call before fork(); parent and child then share the page writable. */
static inline void *sys_shm(void) {
    void *ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(44)
        : "memory"
    );
    return ret;
}

/* Counting semaphores (global ids shared across processes). sem_init sets the
 * count (SYS_SEM_INIT=45); sem_wait blocks until positive then decrements
 * (46); sem_post increments and wakes a waiter (47). Each returns 0 or -1. */
static inline int sys_sem_init(int id, int value) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(45), "b"(id), "c"(value) : "memory");
    return ret;
}
static inline int sys_sem_wait(int id) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(46), "b"(id) : "memory");
    return ret;
}
static inline int sys_sem_post(int id) {
    int ret;
    __asm__ volatile("int $0x80" : "=a"(ret) : "a"(47), "b"(id) : "memory");
    return ret;
}

/* CPU ticks consumed by this process (SYS_CPUTIME = 43). */
static inline int sys_cputime(void) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(43)
        : "memory"
    );
    return ret;
}

#define ENV_VAL_MAX 64   /* matches the kernel's per-variable value limit */

/* Set (or replace) an environment variable (SYS_SETENV = 41). Returns 0/-1. */
static inline int sys_setenv(const char *key, const char *val) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(41), "b"(key), "c"(val)
        : "memory"
    );
    return ret;
}

/* Read an environment variable into `buf` (SYS_GETENV = 42). Returns the value
 * length, or -1 if the key is unset. */
static inline int sys_getenv(const char *key, char *buf, int size) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(42), "b"(key), "c"(buf), "d"(size)
        : "memory"
    );
    return ret;
}

/* Create a pipe (SYS_PIPE = 36): fds[0] = read end, fds[1] = write end. 0/-1. */
static inline int sys_pipe(int fds[2]) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(36), "b"(fds)
        : "memory"
    );
    return ret;
}

/* Duplicate oldfd onto newfd (SYS_DUP2 = 37). A pipe end may target stdin (0)
 * or stdout (1). Returns newfd / -1. */
static inline int sys_dup2(int oldfd, int newfd) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(37), "b"(oldfd), "c"(newfd)
        : "memory"
    );
    return ret;
}

/* Duplicate oldfd into the lowest-numbered available descriptor (SYS_DUP = 52).
 * The new descriptor has an independent ownership reference and returns -1 if
 * oldfd is invalid or the per-process descriptor table is full. */
static inline int sys_dup(int oldfd) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(52), "b"(oldfd)
        : "memory"
    );
    return ret;
}

/* Block until a signal arrives (SYS_PAUSE = 34). Always returns -1. */
static inline int sys_pause(void) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(34)
        : "memory"
    );
    return ret;
}

static inline int sys_sleep(int ticks) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(12), "b"(ticks)
        : "memory"
    );
    return ret;
}

static inline int sys_create(const char *name) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(13), "b"(name)
        : "memory"
    );
    return ret;
}

static inline int sys_write_file(int fd, const char *buf, int len) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(14), "b"(fd), "c"(buf), "d"(len)
        : "memory"
    );
    return ret;
}

static inline int sys_unlink(const char *name) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(15), "b"(name)
        : "memory"
    );
    return ret;
}

static inline int sys_seek(int fd, int offset, int whence) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(16), "b"(fd), "c"(offset), "d"(whence)
        : "memory"
    );
    return ret;
}

static inline int sys_mkdir(const char *path) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(17), "b"(path)
        : "memory"
    );
    return ret;
}

static inline int sys_rmdir(const char *path) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(18), "b"(path)
        : "memory"
    );
    return ret;
}

/* File metadata. type: 1 = regular file, 2 = directory (matches FS_FILE/DIR). */
struct ustat {
    unsigned int size;
    unsigned int type;
    unsigned int inode;
};

#define S_ISDIR(t) ((t) == 2)
#define S_ISREG(t) ((t) == 1)

/* Stat a path (SYS_STAT = 31). Returns 0, fills *st, or -1 if not found. */
static inline int sys_stat(const char *path, struct ustat *st) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(31), "b"(path), "c"(st)
        : "memory"
    );
    return ret;
}

/* Stat an open file descriptor (SYS_FSTAT = 32). Returns 0 / -1. */
static inline int sys_fstat(int fd, struct ustat *st) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(32), "b"(fd), "c"(st)
        : "memory"
    );
    return ret;
}

/* Change the process working directory (SYS_CHDIR = 29). Returns 0 / -1. */
static inline int sys_chdir(const char *path) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(29), "b"(path)
        : "memory"
    );
    return ret;
}

/* Read the process working directory into buf (SYS_GETCWD = 30). Returns the
 * length, or -1 on failure. */
static inline int sys_getcwd(char *buf, int size) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(30), "b"(buf), "c"(size)
        : "memory"
    );
    return ret;
}

static inline int sys_readdir(const char *path, int index, char *buf) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(19), "b"(path), "c"(index), "d"(buf)
        : "memory"
    );
    return ret;
}

/* Spawn a child process with explicit argv (SYS_SPAWN_ARGV = 20).
 * Returns child PID on success; caller must sys_wait() for it. */
static inline int sys_spawn_argv(int argc, const char **argv) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(20), "b"(argv), "c"(argc)
        : "memory"
    );
    return ret;
}

/* Spawn a child with explicit argv and wait for it to finish (SYS_EXEC_ARGV = 21).
 * Returns the child's exit status, or -1 if the program could not be loaded. */
static inline int sys_exec_argv(int argc, const char **argv) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(21), "b"(argv), "c"(argc)
        : "memory"
    );
    return ret;
}

#define SIGINT   2
#define SIGKILL  9
#define SIGUSR1 10
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19

/* Schedule SIGALRM after `ticks` timer ticks (100/sec; 0 cancels). Returns the
 * ticks left on any previous alarm, or -1 if ticks exceeds INT32_MAX
 * (SYS_ALARM = 33). */
static inline int sys_alarm(unsigned int ticks) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(33), "b"(ticks)
        : "memory"
    );
    return ret;
}

/* Send a signal to a process (SYS_KILL = 25). Returns 0 on success. */
static inline int sys_kill(int pid, int signum) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(25), "b"(pid), "c"(signum)
        : "memory"
    );
    return ret;
}

/* Install a handler for `signum` (SYS_SIGNAL = 23). handler may be a function,
 * SIG_DFL (0, terminate) or SIG_IGN (1, ignore). Returns 0 on success. */
extern void __sig_trampoline(void);
static inline int sys_signal(int signum, void (*handler)(int)) {
    int ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(23), "b"(signum), "c"(handler), "d"(&__sig_trampoline)
        : "memory"
    );
    return ret;
}

/* Grow/query the user heap (SYS_SBRK = 22). Returns the previous break on
 * success, or (void *)-1 on failure. Backend for the umalloc allocator. */
static inline void *sys_sbrk(int increment) {
    void *ret;
    __asm__ volatile(
        "int $0x80"
        : "=a"(ret)
        : "a"(22), "b"(increment)
        : "memory"
    );
    return ret;
}

__attribute__((noreturn))
static inline void sys_exit(int code) {
    /* The "memory" clobber is load-bearing: without it the compiler may treat
     * stores before exit() as dead (nothing in THIS program reads them again)
     * and delete them -- but threads, shared memory, and fault tests rely on
     * those stores actually reaching memory before the process ends. */
    __asm__ volatile(
        "int $0x80"
        :: "a"(3), "b"(code)
        : "memory"
    );
    __builtin_unreachable();
}

/* ---- Output helpers (depend on sys_write above) -------------------------- */

/* Unsigned decimal integer to stdout. */
static inline void write_uint(unsigned int n) {
    char buf[12];
    int i = 11;
    buf[i] = '\0';
    if (n == 0) { sys_write("0", 1); return; }
    while (n > 0) {
        buf[--i] = (char)('0' + n % 10);
        n /= 10;
    }
    sys_write(buf + i, 11 - i);
}

/* Signed decimal integer to stdout. */
static inline void write_int(int n) {
    if (n < 0) { sys_write("-", 1); write_uint((unsigned int)(-n)); }
    else        { write_uint((unsigned int)n); }
}

#endif
