#ifndef SYSCALL_H
#define SYSCALL_H

#include <stddef.h>
#include <stdint.h>
#include "isr.h"

enum syscall_number {
    SYS_WRITE     = 1,
    SYS_READ      = 2,
    SYS_EXIT      = 3,
    SYS_EXEC      = 4,
    SYS_OPEN      = 5,
    SYS_READ_FILE = 6,
    SYS_CLOSE     = 7,
    SYS_SPAWN     = 8,
    SYS_WAIT      = 9,
    SYS_YIELD     = 10,
    SYS_GETPID    = 11,
    SYS_SLEEP     = 12,
    SYS_CREATE    = 13,
    SYS_WRITE_FILE = 14,
    SYS_UNLINK    = 15,
    SYS_SEEK      = 16,
    SYS_MKDIR     = 17,
    SYS_RMDIR     = 18,
    SYS_READDIR     = 19,
    SYS_SPAWN_ARGV  = 20,
    SYS_EXEC_ARGV   = 21,  /* spawn child with argv, then wait for it */
    SYS_SBRK        = 22,  /* grow/query the user heap (Unix sbrk semantics) */
    SYS_SIGNAL      = 23,  /* install a signal handler */
    SYS_SIGRETURN   = 24,  /* return from a signal handler (used by trampoline) */
    SYS_KILL        = 25,  /* send a signal to a process */
    SYS_FORK        = 26,  /* clone the current process */
    SYS_EXECV       = 27,  /* replace the current image with a new program */
    SYS_WAITPID     = 28,  /* wait for a specific or any child (with WNOHANG) */
    SYS_CHDIR       = 29,  /* change the process working directory */
    SYS_GETCWD      = 30,  /* read the process working directory */
    SYS_STAT        = 31,  /* file metadata by path */
    SYS_FSTAT       = 32,  /* file metadata by open fd */
    SYS_ALARM       = 33,  /* schedule SIGALRM after N timer ticks */
    SYS_PAUSE       = 34,  /* block until a signal arrives */
    SYS_GETPPID     = 35,  /* parent process id */
    SYS_PIPE        = 36,  /* create a pipe, returning read/write fds */
    SYS_DUP2        = 37,  /* duplicate a descriptor onto another */
    SYS_GETPROCS    = 38,  /* snapshot the process table */
    SYS_UPTIME      = 39,  /* timer ticks since boot (100 Hz monotonic clock) */
    SYS_TIME        = 40,  /* wall-clock date/time from the CMOS RTC */
    SYS_SETENV      = 41,  /* set/replace an environment variable */
    SYS_GETENV      = 42,  /* read an environment variable */
    SYS_CPUTIME     = 43,  /* timer ticks of CPU this process has consumed */
    SYS_SHM         = 44,  /* map a shared page (inherited writable across fork) */
    SYS_SEM_INIT    = 45,  /* initialise a global counting semaphore */
    SYS_SEM_WAIT    = 46,  /* P: block until positive, then decrement */
    SYS_SEM_POST    = 47,  /* V: increment and wake a waiter */
    SYS_MMAP        = 48,  /* allocate demand-paged pages in the mmap region */
    SYS_THREAD_CREATE = 49, /* spawn a thread sharing this address space */
    SYS_THREAD_JOIN   = 50, /* block until all threads have exited */
    SYS_MUNMAP        = 51, /* release pages previously obtained from SYS_MMAP */
    SYS_DUP           = 52, /* duplicate a descriptor into the lowest free fd */
};

/* Keep kernel names distinct from the SEEK_* macros provided by hosted C
 * libraries and modelled by tools such as cppcheck.  The ABI values remain
 * the conventional 0/1/2 exposed to userspace. */
enum seek_whence {
    SYS_SEEK_SET = 0,
    SYS_SEEK_CUR = 1,
    SYS_SEEK_END = 2,
};

struct process;

int32_t sys_write(const char *buffer, size_t count);
int32_t sys_read(char *buffer, size_t count);
int32_t sys_open(const char *name);
int32_t sys_read_file(int32_t fd, char *buffer, size_t count);
int32_t sys_write_file(int32_t fd, const char *buffer, size_t count);
int32_t sys_close(int32_t fd);
int32_t sys_create(const char *name);
int32_t sys_unlink(const char *name);
int32_t sys_seek(int32_t fd, int32_t offset, int32_t whence);
int32_t sys_sbrk(int32_t increment);
int32_t sys_mkdir(const char *path);
int32_t sys_rmdir(const char *path);
int32_t sys_chdir(const char *path);
int32_t sys_getcwd(char *buffer, size_t size);
int32_t sys_stat(const char *path, void *statbuf);
int32_t sys_fstat(int32_t fd, void *statbuf);
int32_t sys_readdir(const char *path, uint32_t index, char *buffer);
void sys_exit(int32_t status) __attribute__((noreturn));
void syscall_close_user_files(struct process *process);
/* Duplicate an open file table from one process to another (used by fork). */
void syscall_copy_user_files(struct process *parent, struct process *child);
void syscall_install(void);

/* Deliver a pending signal when returning to user mode. Called at the end of
 * the IRQ and syscall handlers with the about-to-be-restored register frame. */
void signal_deliver(registers_t *regs);

#endif
