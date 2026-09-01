#include "syscall.h"
#include "elf_loader.h"
#include "fs.h"
#include "kb.h"
#include "paging.h"
#include "pipe.h"
#include "process.h"
#include "rtc.h"
#include "sem.h"
#include "task.h"
#include "timer.h"
#include "utils.h"
#include "vga.h"

#define MAX_OPEN_FILES 8
#define FIRST_USER_FD  3
#define MAX_USER_STRING 128
#define MAX_SPAWN_ARGS  MAX_ARGS   /* from elf_loader.h */
#define USER_SHM_BASE 0x3F0000U
#define USER_SHM_TOP  (USER_SHM_BASE + 0x1000U)

/* A user file descriptor (index FIRST_USER_FD + slot) is either a file or one
 * end of a pipe. (fd 0/1 are handled by the process stdin/stdout fields.) */
typedef enum { OF_NONE = 0, OF_FILE, OF_PIPE_R, OF_PIPE_W } of_kind_t;

typedef struct {
    of_kind_t  kind;
    fs_node_t *node;    /* OF_FILE */
    uint32_t   offset;  /* OF_FILE */
    pipe_t    *pipe;    /* OF_PIPE_* */
} open_file_t;

static open_file_t open_files[MAX_PROCESSES][MAX_OPEN_FILES];

static open_file_t *current_open_files(void) {
    process_t *process = process_get_current();

    if (!process || process->slot >= MAX_PROCESSES) return NULL;
    return open_files[process->slot];
}

/* Convert a user descriptor to its table slot only after proving the
 * subtraction is in range.  fd comes directly from a user register, so doing
 * `fd - FIRST_USER_FD` first is signed overflow for values such as INT32_MIN. */
static int user_fd_index(int32_t fd) {
    if (fd < FIRST_USER_FD || fd >= FIRST_USER_FD + MAX_OPEN_FILES) return -1;
    return fd - FIRST_USER_FD;
}

/* Reserve a free descriptor slot; returns its fd number or -1. */
static int32_t alloc_fd(open_file_t *files, of_kind_t kind,
                        fs_node_t *node, pipe_t *pipe) {
    if (!files) return -1;
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (files[i].kind == OF_NONE) {
            files[i].kind = kind;
            files[i].node = node;
            files[i].offset = 0;
            files[i].pipe = pipe;
            return FIRST_USER_FD + i;
        }
    }
    return -1;
}

/* Release a descriptor, dropping its underlying reference. */
static void close_fd_entry(open_file_t *entry) {
    switch (entry->kind) {
        case OF_FILE:   if (entry->node) close_fs(entry->node); break;
        case OF_PIPE_R: pipe_close_read(entry->pipe); break;
        case OF_PIPE_W: pipe_close_write(entry->pipe); break;
        default: break;
    }
    entry->kind = OF_NONE;
    entry->node = NULL;
    entry->offset = 0;
    entry->pipe = NULL;
}

static int32_t open_user_file(fs_node_t *node) {
    open_file_t *files = current_open_files();
    int32_t fd = alloc_fd(files, OF_FILE, node, NULL);

    if (fd >= 0) open_fs(node);
    return fd;
}

/* Return the exclusive end of the user-owned region containing `start`.
 * User memory is split between the low image/heap/stack window, the dedicated
 * shared page, and the extended mmap window; ranges may not bridge the gaps. */
static uint32_t user_region_top(uint32_t start) {
    if (start >= USER_LOAD_BASE && start < USER_STACK_TOP)
        return USER_STACK_TOP;
    if (start >= USER_SHM_BASE && start < USER_SHM_TOP)
        return USER_SHM_TOP;
    if (start >= USER_EXT_BASE && start < USER_EXT_TOP)
        return USER_EXT_TOP;
    return 0;
}

static int user_buffer_valid(const void *buffer, size_t count) {
    uint32_t start = (uint32_t)buffer;
    uint32_t top;

    if (count == 0) return 1;
    top = user_region_top(start);
    if (top == 0) return 0;
    return count <= top - start &&
           paging_user_range_mapped(start, count);
}

/* Resolve a user-supplied path against the current process's working directory
 * into a clean absolute path. Returns 0 on success. */
static int resolve_user_path(const char *path, char *out) {
    process_t *process = process_get_current();
    const char *cwd = (process && process->cwd[0]) ? process->cwd : "/";

    return vfs_resolve_path(cwd, path, out);
}

static int user_string_valid(const char *string) {
    uint32_t start = (uint32_t)string;
    uint32_t limit;
    uint32_t top = user_region_top(start);

    if (top == 0) return 0;

    limit = top - start;
    if (limit > MAX_USER_STRING) limit = MAX_USER_STRING;

    for (uint32_t i = 0; i < limit; i++) {
        if (!paging_user_range_mapped(start + i, 1)) return 0;
        if (string[i] == '\0') return 1;
    }
    return 0;
}

int32_t sys_write(const char *buffer, size_t count) {
    process_t *process = process_get_current();

    if (!user_buffer_valid(buffer, count)) return -1;

    /* stdout connected to a pipeline: write into the pipe (may block). */
    if (process && process->stdout_pipe) {
        return (int32_t)pipe_write(process->stdout_pipe,
                                   (const uint8_t *)buffer, count);
    }

    /* stdout redirected to a file: append at the running offset. */
    if (process && process->stdout_node) {
        uint32_t written = write_fs(process->stdout_node,
                                    process->stdout_offset,
                                    count, (uint8_t *)buffer);
        process->stdout_offset += written;
        return (int32_t)written;
    }

    terminal_write(buffer, count);
    return (int32_t)count;
}

int32_t sys_read(char *buffer, size_t count) {
    process_t *process = process_get_current();

    if (!user_buffer_valid(buffer, count)) return -1;

    /* stdin connected to a pipeline: read from the pipe (may block, 0 = EOF). */
    if (process && process->stdin_pipe) {
        return (int32_t)pipe_read(process->stdin_pipe,
                                  (uint8_t *)buffer, count);
    }

    /* stdin redirected from a file: read sequentially, 0 means EOF. */
    if (process && process->stdin_node) {
        uint32_t bytes = read_fs(process->stdin_node,
                                 process->stdin_offset,
                                 count, (uint8_t *)buffer);
        process->stdin_offset += bytes;
        return (int32_t)bytes;
    }

    return (int32_t)keyboard_read(buffer, count);
}

int32_t sys_open(const char *name) {
    char abs[FS_MAX_PATH];
    fs_node_t *node;

    if (!user_string_valid(name)) return -1;
    if (resolve_user_path(name, abs) != 0) return -1;

    node = resolve_fs(abs);
    if (!node || node->flags != FS_FILE) return -1;
    return open_user_file(node);
}

int32_t sys_create(const char *name) {
    char abs[FS_MAX_PATH];
    fs_node_t *node;
    int32_t fd;

    if (!user_string_valid(name)) return -1;
    if (resolve_user_path(name, abs) != 0) return -1;

    node = create_fs(abs);
    if (!node) return -1;

    fd = open_user_file(node);
    if (fd < 0) unlink_fs(abs);
    return fd;
}

int32_t sys_read_file(int32_t fd, char *buffer, size_t count) {
    int index = user_fd_index(fd);
    open_file_t *files = current_open_files();
    uint32_t bytes_read;

    if (index < 0 || !files) return -1;
    if (!user_buffer_valid(buffer, count)) return -1;

    if (files[index].kind == OF_PIPE_R)
        return (int32_t)pipe_read(files[index].pipe, (uint8_t *)buffer, count);
    if (files[index].kind != OF_FILE) return -1;

    bytes_read = read_fs(files[index].node,
                         files[index].offset,
                         count,
                         (uint8_t *)buffer);
    files[index].offset += bytes_read;
    return (int32_t)bytes_read;
}

int32_t sys_write_file(int32_t fd, const char *buffer, size_t count) {
    int index = user_fd_index(fd);
    open_file_t *files = current_open_files();
    uint32_t bytes_written;

    if (index < 0 || !files) return -1;
    if (!user_buffer_valid(buffer, count)) return -1;

    if (files[index].kind == OF_PIPE_W)
        return (int32_t)pipe_write(files[index].pipe,
                                   (const uint8_t *)buffer, count);
    if (files[index].kind != OF_FILE || !files[index].node->write) return -1;
    if (count == 0) return 0;

    bytes_written = write_fs(files[index].node,
                             files[index].offset,
                             count,
                             (uint8_t *)buffer);
    if (bytes_written == 0) return -1;

    files[index].offset += bytes_written;
    return (int32_t)bytes_written;
}

int32_t sys_unlink(const char *name) {
    char abs[FS_MAX_PATH];
    if (!user_string_valid(name)) return -1;
    if (resolve_user_path(name, abs) != 0) return -1;
    return unlink_fs(abs);
}

int32_t sys_chdir(const char *path) {
    process_t *process = process_get_current();
    char abs[FS_MAX_PATH];
    fs_node_t *node;

    if (!process || !user_string_valid(path)) return -1;
    if (resolve_user_path(path, abs) != 0) return -1;

    node = resolve_fs(abs);
    if (!node || node->flags != FS_DIRECTORY) return -1;

    strcpy(process->cwd, abs);   /* abs fits: FS_MAX_PATH == PROCESS_CWD_MAX */
    return 0;
}

int32_t sys_getcwd(char *buffer, size_t size) {
    process_t *process = process_get_current();
    const char *cwd;
    size_t len;

    if (!process) return -1;
    cwd = process->cwd[0] ? process->cwd : "/";
    len = strlen(cwd);
    if (!user_buffer_valid(buffer, size) || size < len + 1) return -1;

    for (size_t i = 0; i <= len; i++) buffer[i] = cwd[i];
    return (int32_t)len;
}

int32_t sys_mkdir(const char *path) {
    char abs[FS_MAX_PATH];
    if (!user_string_valid(path)) return -1;
    if (resolve_user_path(path, abs) != 0) return -1;
    return mkdir_fs(abs);
}

int32_t sys_rmdir(const char *path) {
    char abs[FS_MAX_PATH];
    if (!user_string_valid(path)) return -1;
    if (resolve_user_path(path, abs) != 0) return -1;
    return rmdir_fs(abs);
}

int32_t sys_readdir(const char *path, uint32_t index, char *buffer) {
    char abs[FS_MAX_PATH];
    fs_node_t *node;
    dirent_t *entry;

    if (!user_string_valid(path) ||
        !user_buffer_valid(buffer, sizeof(entry->name))) {
        return -1;
    }
    if (resolve_user_path(path, abs) != 0) return -1;

    node = resolve_fs(abs);
    if (!node || node->flags != FS_DIRECTORY) return -1;

    entry = readdir_fs(node, index);
    if (!entry) return 0;

    strcpy(buffer, entry->name);
    return 1;
}

int32_t sys_seek(int32_t fd, int32_t offset, int32_t whence) {
    int index = user_fd_index(fd);
    open_file_t *files = current_open_files();
    int64_t base;
    int64_t position;

    if (index < 0) return -1;
    if (!files || files[index].kind != OF_FILE) return -1;

    switch (whence) {
        case SYS_SEEK_SET:
            base = 0;
            break;
        case SYS_SEEK_CUR:
            base = files[index].offset;
            break;
        case SYS_SEEK_END:
            base = files[index].node->length;
            break;
        default:
            return -1;
    }

    position = base + offset;
    if (position < 0 || position > 0x7FFFFFFF) return -1;

    files[index].offset = (uint32_t)position;
    return (int32_t)position;
}

/* Fill the user stat buffer (size, type, inode) from a VFS node. */
static void fill_stat(uint32_t *out, const fs_node_t *node) {
    out[0] = node->length;
    out[1] = node->flags;
    out[2] = node->inode;
}

int32_t sys_stat(const char *path, void *statbuf) {
    char abs[FS_MAX_PATH];
    fs_node_t *node;

    if (!user_string_valid(path) || !user_buffer_valid(statbuf, 12)) return -1;
    if (resolve_user_path(path, abs) != 0) return -1;

    node = resolve_fs(abs);
    if (!node) return -1;
    fill_stat((uint32_t *)statbuf, node);
    return 0;
}

int32_t sys_fstat(int32_t fd, void *statbuf) {
    int index = user_fd_index(fd);
    open_file_t *files = current_open_files();

    if (index < 0) return -1;
    if (!files || files[index].kind != OF_FILE) return -1;
    if (!user_buffer_valid(statbuf, 12)) return -1;

    fill_stat((uint32_t *)statbuf, files[index].node);
    return 0;
}

/* Create a pipe; write the read fd to fds[0] and the write fd to fds[1]. */
static int32_t sys_pipe(int32_t *user_fds) {
    open_file_t *files = current_open_files();
    pipe_t *p;
    int32_t rfd, wfd;

    if (!files || !user_buffer_valid(user_fds, 2 * sizeof(int32_t))) return -1;

    p = pipe_create();
    if (!p) return -1;

    rfd = alloc_fd(files, OF_PIPE_R, NULL, p);
    if (rfd < 0) {
        pipe_close_read(p);
        pipe_close_write(p);
        return -1;
    }
    wfd = alloc_fd(files, OF_PIPE_W, NULL, p);
    if (wfd < 0) {
        close_fd_entry(&files[rfd - FIRST_USER_FD]);   /* releases the read end */
        pipe_close_write(p);                           /* releases the write end */
        return -1;
    }

    user_fds[0] = rfd;
    user_fds[1] = wfd;
    return 0;
}

/* Duplicate oldfd into the lowest-numbered free descriptor slot. The duplicate
 * gets its own ownership reference and starts with the source descriptor's
 * current file offset. Pipe read/write ends retain the matching end only. */
static int32_t sys_dup(int32_t oldfd) {
    open_file_t *files = current_open_files();
    int oidx;
    open_file_t src;

    if (!files || oldfd < FIRST_USER_FD ||
        oldfd >= FIRST_USER_FD + MAX_OPEN_FILES) return -1;
    oidx = oldfd - FIRST_USER_FD;
    if (files[oidx].kind == OF_NONE) return -1;
    src = files[oidx];

    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (files[i].kind != OF_NONE) continue;
        files[i] = src;
        switch (src.kind) {
            case OF_FILE:   if (src.node) open_fs(src.node); break;
            case OF_PIPE_R: pipe_ref_read(src.pipe); break;
            case OF_PIPE_W: pipe_ref_write(src.pipe); break;
            default: break;
        }
        return FIRST_USER_FD + i;
    }
    return -1;
}

/* Duplicate descriptor oldfd onto newfd. A pipe end may be placed on stdin (0)
 * or stdout (1); any descriptor may be copied to another table slot (>= 3). */
static int32_t sys_dup2(int32_t oldfd, int32_t newfd) {
    process_t *process = process_get_current();
    open_file_t *files = current_open_files();
    int oidx = user_fd_index(oldfd);
    open_file_t src;

    if (!process || !files) return -1;
    if (oidx < 0) return -1;
    if (files[oidx].kind == OF_NONE) return -1;
    src = files[oidx];

    if (newfd == 1) {                       /* stdout <- pipe write end or file */
        if (src.kind != OF_PIPE_W && src.kind != OF_FILE) return -1;
        if (process->stdout_pipe) {
            pipe_close_write(process->stdout_pipe);
            process->stdout_pipe = NULL;
        }
        if (src.kind == OF_PIPE_W) {
            if (process->stdout_node) close_fs(process->stdout_node);
            process->stdout_pipe = src.pipe;
            process->stdout_node = NULL;
            pipe_ref_write(src.pipe);
        } else if (src.kind == OF_FILE) {
            /* Take a reference: the caller typically closes oldfd right after
             * (the usual dup2-then-close idiom), which would otherwise leave
             * stdout pointing at an unreferenced node that unlink() is free to
             * kfree() -- a use-after-free on the next write. Released in
             * process_finish_exit(). */
            if (process->stdout_node) close_fs(process->stdout_node);
            process->stdout_node = src.node;
            if (src.node) open_fs(src.node);
            process->stdout_offset = src.offset;
        }
        return newfd;
    }
    if (newfd == 0) {                       /* stdin <- pipe read end or file */
        if (src.kind != OF_PIPE_R && src.kind != OF_FILE) return -1;
        if (process->stdin_pipe) {
            pipe_close_read(process->stdin_pipe);
            process->stdin_pipe = NULL;
        }
        if (src.kind == OF_PIPE_R) {
            if (process->stdin_node) close_fs(process->stdin_node);
            process->stdin_pipe = src.pipe;
            process->stdin_node = NULL;
            pipe_ref_read(src.pipe);
        } else if (src.kind == OF_FILE) {
            if (process->stdin_node) close_fs(process->stdin_node);
            process->stdin_node = src.node;
            if (src.node) open_fs(src.node);
            process->stdin_offset = src.offset;
        }
        return newfd;
    }
    if (newfd >= FIRST_USER_FD) {           /* copy into another table slot */
        int nidx = user_fd_index(newfd);
        if (nidx < 0) return -1;
        if (newfd == oldfd) return newfd;
        close_fd_entry(&files[nidx]);
        files[nidx] = src;
        switch (src.kind) {
            case OF_FILE:   if (src.node) open_fs(src.node); break;
            case OF_PIPE_R: pipe_ref_read(src.pipe); break;
            case OF_PIPE_W: pipe_ref_write(src.pipe); break;
            default: break;
        }
        return newfd;
    }
    return -1;
}

int32_t sys_close(int32_t fd) {
    int index = user_fd_index(fd);
    open_file_t *files = current_open_files();

    if (index < 0) return -1;
    if (!files || files[index].kind == OF_NONE) return -1;

    close_fd_entry(&files[index]);
    return 0;
}

void syscall_close_user_files(process_t *process) {
    open_file_t *files;

    if (!process || process->slot >= MAX_PROCESSES) return;
    files = open_files[process->slot];

    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (files[i].kind != OF_NONE) close_fd_entry(&files[i]);
    }
}

void syscall_copy_user_files(process_t *parent, process_t *child) {
    open_file_t *src, *dst;

    if (!parent || !child ||
        parent->slot >= MAX_PROCESSES || child->slot >= MAX_PROCESSES) {
        return;
    }
    src = open_files[parent->slot];
    dst = open_files[child->slot];

    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        dst[i] = src[i];
        switch (dst[i].kind) {                    /* bump each shared reference */
            case OF_FILE:   if (dst[i].node) open_fs(dst[i].node); break;
            case OF_PIPE_R: pipe_ref_read(dst[i].pipe); break;
            case OF_PIPE_W: pipe_ref_write(dst[i].pipe); break;
            default: break;
        }
    }
}

/* Grow or query the current process's heap (Unix sbrk semantics):
 * returns the previous break, or -1 on failure. Pages are mapped on demand. */
int32_t sys_sbrk(int32_t increment) {
    process_t *process = process_get_current();
    uint32_t old_break;

    if (!process) return -1;
    old_break = process->heap_break;

    if (increment == 0) return (int32_t)old_break;

    if (increment < 0) {
        /* Negate in unsigned arithmetic: `-increment` is undefined when
         * increment is INT32_MIN, and this value comes straight from a user
         * register. The unsigned form yields the correct magnitude for every
         * negative input. */
        uint32_t dec = 0u - (uint32_t)increment;
        if (dec > old_break - process->heap_start) return -1;
        process->heap_break = old_break - dec;
        return (int32_t)old_break;
    }

    /* Grow: just move the break. Heap pages are paged in on demand when the
     * program first touches them (see the page-fault handler). */
    if ((uint32_t)increment > USER_HEAP_MAX - old_break) return -1;
    process->heap_break = old_break + (uint32_t)increment;
    return (int32_t)old_break;
}

/* Clone the current process. Returns the child pid to the parent; the child
 * resumes here with eax = 0 (set up via the saved fork frame). */
static int32_t sys_fork(registers_t *regs) {
    process_t *parent = process_get_current();
    address_space_t *clone;
    fork_frame_t frame;
    int32_t pid;

    if (!parent) return -1;

    clone = paging_clone_address_space(parent->address_space);
    if (!clone) return -1;

    frame.eip = regs->eip;
    frame.eflags = regs->eflags;
    frame.useresp = regs->useresp;
    frame.eax = 0;                /* child sees fork() == 0 */
    frame.ebx = regs->ebx;
    frame.ecx = regs->ecx;
    frame.edx = regs->edx;
    frame.esi = regs->esi;
    frame.edi = regs->edi;
    frame.ebp = regs->ebp;

    pid = process_fork(parent, clone, &frame);
    if (pid < 0) {
        paging_destroy_user_address_space(clone);
        return -1;
    }
    return pid;
}

/* Replace the current process's image with a new program (Unix execv).
 * On success it sets `regs` to enter the new program and returns 0; the old
 * image never resumes. On failure returns -1 and the caller is unchanged. */
static int32_t sys_execv(registers_t *regs, int argc, const char **user_argv) {
    char bufs[MAX_SPAWN_ARGS][MAX_USER_STRING + 1];
    const char *argv[MAX_SPAWN_ARGS];
    uint32_t entry, user_esp, heap_base;
    address_space_t *space;
    int i;

    if (argc <= 0 || argc > MAX_SPAWN_ARGS ||
        !user_buffer_valid(user_argv, (uint32_t)argc * sizeof(char *))) {
        return -1;
    }
    for (i = 0; i < argc; i++) {
        if (!user_string_valid(user_argv[i])) return -1;
        strcpy(bufs[i], user_argv[i]);
        argv[i] = bufs[i];
    }

    /* Build the new image first; if that fails the caller is untouched. */
    space = elf_load_image(argv[0], argc, argv, &entry, &user_esp, &heap_base);
    if (!space) return -1;

    if (process_exec_reset(space, heap_base, argv[0]) != 0) {
        paging_destroy_user_address_space(space);
        return -1;
    }

    /* Enter the new program with a fresh register set on its own stack. */
    regs->eip = entry;
    regs->useresp = user_esp;
    regs->eax = regs->ebx = regs->ecx = regs->edx = 0;
    regs->esi = regs->edi = regs->ebp = 0;
    regs->eflags |= 0x200;
    return 0;
}

/* waitpid(pid, *status, options): reap a child. options bit 0 = WNOHANG. */
static int32_t sys_waitpid(int32_t pid, int32_t *user_status, int options) {
    int32_t status = 0;
    int32_t rpid;

    if (user_status && !user_buffer_valid(user_status, sizeof(int32_t))) return -1;
    rpid = process_waitpid(pid, &status, options & 1);
    if (rpid > 0 && user_status) *user_status = status;
    return rpid;
}

/* Arrange for SIGALRM after `ticks` timer ticks (0 cancels). Returns the ticks
 * remaining on any previous alarm. */
static int32_t sys_alarm(uint32_t ticks) {
    process_t *process = process_get_current();
    uint32_t now = timer_get_ticks();
    uint32_t remaining = 0;

    if (!process) return 0;
    /* Signed modular deadline comparisons are unambiguous only within half of
     * the uint32_t range. Match timer_sleep's bound and preserve any old alarm
     * when a replacement is rejected. */
    if (ticks > 0x7FFFFFFFU) return -1;
    if (process->alarm_active &&
        (int32_t)(process->alarm_tick - now) > 0) {
        remaining = process->alarm_tick - now;
    }
    if (ticks == 0) {
        process->alarm_active = 0;
        process->alarm_tick = 0;
    } else {
        process->alarm_tick = now + ticks;
        process->alarm_active = 1;
    }
    return (int32_t)remaining;
}

/* Copy a snapshot of the process table into a user buffer (array of
 * process_info_t). Returns the number of entries written. */
static int32_t sys_getprocs(void *user_buf, int max) {
    process_info_t snap[MAX_PROCESSES];
    uint32_t count;

    if (max <= 0) return -1;
    if (max > MAX_PROCESSES) max = MAX_PROCESSES;
    if (!user_buffer_valid(user_buf, (uint32_t)max * sizeof(process_info_t)))
        return -1;

    count = process_snapshot(snap, (uint32_t)max);
    memcpy(user_buf, snap, count * sizeof(process_info_t));
    return (int32_t)count;
}

/* Monotonic timer ticks since boot (the PIT runs at 100 Hz). */
static int32_t sys_uptime(void) {
    return (int32_t)timer_get_ticks();
}

/* CPU time (timer ticks) consumed by the calling process. */
static int32_t sys_cputime(void) {
    return (int32_t)process_get_cpu_ticks();
}

/* Map (once) a page of shared memory that survives fork as a writable shared
 * mapping. Returns its fixed user address, or 0 on failure. Allocate it before
 * forking so the child inherits the same physical frame. */
static int32_t sys_shm(void) {
    if (!paging_share_page(USER_SHM_BASE)) return 0;
    return (int32_t)USER_SHM_BASE;
}

/* Reserve `npages` of demand-paged memory in the process's mmap region (above
 * the kernel identity map, so it can exceed the old 4MB low window). Returns the
 * base address of the new chunk, or 0 if no free run is large enough. */
static int32_t sys_mmap(int npages) {
    process_t *proc = process_get_current();

    if (!proc || npages <= 0) return 0;
    return process_ext_alloc(proc, (uint32_t)npages);
}

/* Release a run of pages previously obtained from sys_mmap: the reservation is
 * dropped (so the addresses can be handed out again) and any frames that were
 * demand-mapped are returned to the physical allocator. Freeing part of a
 * chunk is fine, but every page in the range must currently be reserved. */
static int32_t sys_munmap(uint32_t addr, int npages) {
    process_t *proc = process_get_current();

    if (!proc || npages <= 0) return -1;
    if (process_ext_free(proc, addr, (uint32_t)npages) < 0) return -1;
    for (int i = 0; i < npages; i++)
        paging_unmap_user_page(proc->address_space,
                               addr + (uint32_t)i * 0x1000U);
    return 0;
}

/* Spawn a thread that shares this process's address space (SYS_THREAD_CREATE).
 * A thread terminates by calling exit(), which drops the thread count without
 * tearing down the shared address space. */
static int32_t sys_thread_create(uint32_t entry, uint32_t stack_top) {
    return process_thread_create(entry, stack_top);
}

/* Counting-semaphore syscalls (global ids, shared across processes). */
static int32_t sys_sem_init(int id, int value) { return sem_init(id, value); }
static int32_t sys_sem_wait(int id) { return sem_wait(id); }
static int32_t sys_sem_post(int id) { return sem_post(id); }

/* Fill the user's rtc_time_t buffer with the current CMOS wall-clock time. */
static int32_t sys_time(void *user_buf) {
    rtc_time_t now;

    if (!user_buffer_valid(user_buf, sizeof(rtc_time_t))) return -1;
    rtc_read(&now);
    memcpy(user_buf, &now, sizeof(rtc_time_t));
    return 0;
}

/* Set (or replace) an environment variable for the current process. */
static int32_t sys_setenv(const char *key, const char *val) {
    if (!user_string_valid(key) || !user_string_valid(val)) return -1;
    return process_setenv(key, val);
}

/* Read an environment variable into the user buffer. Returns the value length,
 * or -1 if the key is unset or the arguments are invalid. */
static int32_t sys_getenv(const char *key, char *buf, uint32_t size) {
    if (!user_string_valid(key) || !user_buffer_valid(buf, size)) return -1;
    return process_getenv(key, buf, size);
}

void sys_exit(int32_t status) {
    task_exit(status);
}

/* The EFLAGS bits a ring-3 program may choose for itself: the arithmetic
 * results (CF PF AF ZF SF OF) and the direction flag. Everything else is the
 * kernel's to decide -- see the note in sys_sigreturn. */
#define EFLAGS_USER_MASK   0x00000CD5u
/* IF, so a program can never resume with interrupts disabled, plus the
 * reserved bit 1 which is always set. */
#define EFLAGS_ALWAYS_SET  0x00000202u

/* User context saved on the user stack while a signal handler runs, so that
 * SYS_SIGRETURN can restore the interrupted code exactly. */
typedef struct {
    uint32_t eip, eflags;
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp;
    uint32_t useresp;
} sigcontext_t;

/* Send a signal to a process. SIGKILL is uncatchable (forced kill); any other
 * signal is posted for catchable delivery. Returns 0, or -1 on bad pid/signal. */
static int32_t sys_kill(int pid, int signum) {
    if (pid <= 0 || signum <= 0 || signum >= NSIG) return -1;
    if (signum == SIGKILL) return process_kill(pid);
    return process_send_signal(pid, signum);
}

/* Install a user signal handler: handler 0 = default, 1 = ignore, else addr. */
static int32_t sys_signal(int signum, uint32_t handler, uint32_t trampoline) {
    process_t *process = process_get_current();

    if (!process || signum <= 0 || signum >= NSIG) return -1;
    process->sig_handler[signum] = handler;
    if (trampoline) process->sig_trampoline = trampoline;
    return 0;
}

/* Signal frames normally live in the fixed low user stack, but every thread
 * created by SYS_THREAD_CREATE uses a stack allocated from SYS_MMAP. Accept an
 * extended-stack range only while every page remains reserved by this process;
 * the page-fault handler can demand-map those pages while the kernel builds the
 * frame. */
static int signal_stack_range_valid(const process_t *process, uint32_t start,
                                    uint32_t size) {
    uint32_t end;

    if (!process || size == 0 || size > UINT32_MAX - start) return 0;
    end = start + size;

    if (start >= USER_STACK_BOTTOM && end <= USER_STACK_TOP) return 1;
    if (start < USER_EXT_BASE || end > USER_EXT_TOP) return 0;

    for (uint32_t page = start & ~0xFFFU; page < end; page += 0x1000U) {
        if (!process_ext_reserved(process, page)) return 0;
    }
    return 1;
}

/* Restore the pre-signal context from the user stack (called by the trampoline
 * via SYS_SIGRETURN). regs->useresp points just below the saved context. */
static void sys_sigreturn(registers_t *regs) {
    process_t *process = process_get_current();
    const sigcontext_t *sc;

    if (!process) return;

    /* regs->useresp is a plain user register and SYS_SIGRETURN is reachable by
     * any program issuing int $0x80 directly -- it need not be inside a signal
     * handler at all. Validate that the whole saved-context frame lies in the
     * user stack before dereferencing it. Reading an unmapped or out-of-range
     * address here would fault with CS still 0x08, which the page-fault handler
     * classifies as a kernel fault and answers by halting the entire machine.
     * The stack region itself is demand-paged, so an in-range address is safe
     * to touch. A frame that fails this check means the context is unusable, so
     * terminate just this process (mirrors the write-side check in
     * signal_deliver). */
    if (!signal_stack_range_valid(process, regs->useresp,
                                  4 + sizeof(sigcontext_t))) {
        task_exit(-1);
    }

    sc = (const sigcontext_t *)(regs->useresp + 4);
    regs->eip = sc->eip;
    /* EFLAGS is the one saved field that is not just data: the iret that
     * returns to ring 3 loads it into the real register, and an iret executed
     * at CPL 0 -- which is where the kernel is -- takes the IOPL field from
     * the stack image instead of preserving it. Copying the saved value
     * verbatim therefore lets a program choose its own EFLAGS, and the frame
     * lives in its own stack, so the bounds check above says nothing about the
     * contents. What it could pick:
     *
     *   IOPL=3  in/out on any port from ring 3, and cli/sti -- so either drive
     *           the disk controller behind the filesystem's back, or simply
     *           cli and stop the machine: no timer, no scheduler, no keyboard.
     *   VM      return into virtual-8086 mode.
     *   NT      make the next iret attempt a task switch through the TSS.
     *   IF=0    return to ring 3 with interrupts disabled: the same freeze.
     *
     * Keep only the flags a program legitimately owns -- the arithmetic
     * results and the direction flag, which code after a handler depends on --
     * and supply the rest. TF is excluded as well: single-stepping from ring 3
     * would raise #DB, which this kernel has no handler for. */
    regs->eflags = (sc->eflags & EFLAGS_USER_MASK) | EFLAGS_ALWAYS_SET;
    regs->eax = sc->eax;
    regs->ebx = sc->ebx;
    regs->ecx = sc->ecx;
    regs->edx = sc->edx;
    regs->esi = sc->esi;
    regs->edi = sc->edi;
    regs->ebp = sc->ebp;
    regs->useresp = sc->useresp;
    process->in_signal = 0;
}

void signal_deliver(registers_t *regs) {
    process_t *process = process_get_current();

    /* Only deliver when about to return to user mode, and not already nested. */
    if (!process || regs->cs != 0x1B || process->in_signal) return;
    if (process->sig_pending == 0) return;

    for (int sig = 1; sig < NSIG; sig++) {
        uint32_t handler;
        uint32_t usp;
        sigcontext_t *sc;

        if (!(process->sig_pending & (1u << sig))) continue;
        process->sig_pending &= ~(1u << sig);
        handler = process->sig_handler[sig];

        /* Job control. SIGSTOP is uncatchable: block here (still on the kernel
         * side, about to return to user mode) until SIGCONT arrives. The stop
         * channel is woken by process_send_signal when any signal is posted; we
         * re-block unless that signal was SIGCONT. */
        if (sig == SIGSTOP) {
            process->stopped = 1;
            while (process->stopped) {
                task_block_killable(&process->stopped);
                if (process->sig_pending & (1u << SIGCONT)) {
                    process->sig_pending &= ~(1u << SIGCONT);
                    process->stopped = 0;
                }
            }
            continue;
        }
        /* SIGCONT's default action is simply to resume (a no-op here, since we
         * only reach this code while running); run a handler if one is set. */
        if (sig == SIGCONT && handler <= 1) continue;

        if (handler == 0) {
            /* Default action: SIGCHLD is ignored, everything else terminates. */
            if (sig == SIGCHLD) continue;
            task_exit(-128 - sig);
        }
        if (handler == 1) continue;  /* explicitly ignored */

        /* Build a signal frame on the user stack:
         *   [saved context][signum][trampoline addr] <- new esp
         * regs->useresp is a plain user-mode register: a program can set it
         * to anything before a signal becomes pending. Validate the whole
         * frame stays inside the mapped/demand-pageable stack region before
         * touching it -- otherwise the write below would fault at an address
         * the page-fault handler doesn't recognise as demand-pageable, and
         * since that fault happens from kernel code (CS still 0x08 at the
         * point of the write) it would be treated as a kernel fault and halt
         * the whole system instead of just this process. */
        if (regs->useresp < sizeof(sigcontext_t) + 8 ||
            !signal_stack_range_valid(process,
                                      regs->useresp - sizeof(sigcontext_t) - 8,
                                      sizeof(sigcontext_t) + 8)) {
            task_exit(-128 - sig);
        }

        usp = regs->useresp;
        usp -= sizeof(sigcontext_t);
        sc = (sigcontext_t *)usp;
        sc->eip = regs->eip;
        sc->eflags = regs->eflags;
        sc->eax = regs->eax;
        sc->ebx = regs->ebx;
        sc->ecx = regs->ecx;
        sc->edx = regs->edx;
        sc->esi = regs->esi;
        sc->edi = regs->edi;
        sc->ebp = regs->ebp;
        sc->useresp = regs->useresp;

        usp -= 4;
        *(uint32_t *)usp = (uint32_t)sig;             /* handler argument */
        usp -= 4;
        *(uint32_t *)usp = process->sig_trampoline;   /* handler return addr */

        regs->useresp = usp;
        regs->eip = handler;
        process->in_signal = 1;
        return;  /* one signal per return to user mode */
    }
}

static void syscall_handler(registers_t *regs) {
    /* eax = syscall number, ebx/ecx/edx = parameters */
    switch (regs->eax) {
        case SYS_WRITE:
            regs->eax = (uint32_t)sys_write((const char *)regs->ebx, regs->ecx);
            break;
        case SYS_READ:
            regs->eax = (uint32_t)sys_read((char *)regs->ebx, regs->ecx);
            break;
        case SYS_EXIT:
            sys_exit((int32_t)regs->ebx);
            break;
        case SYS_EXEC:
            if (user_string_valid((const char *)regs->ebx))
                regs->eax = (uint32_t)elf_exec((const char *)regs->ebx);
            else
                regs->eax = (uint32_t)-1;
            break;
        case SYS_OPEN:
            regs->eax = (uint32_t)sys_open((const char *)regs->ebx);
            break;
        case SYS_READ_FILE:
            regs->eax = (uint32_t)sys_read_file((int32_t)regs->ebx,
                                                (char *)regs->ecx,
                                                regs->edx);
            break;
        case SYS_CLOSE:
            regs->eax = (uint32_t)sys_close((int32_t)regs->ebx);
            break;
        case SYS_SPAWN:
            if (user_string_valid((const char *)regs->ebx))
                regs->eax = (uint32_t)elf_spawn((const char *)regs->ebx);
            else
                regs->eax = (uint32_t)-1;
            break;
        case SYS_WAIT:
            regs->eax = (uint32_t)process_wait((int32_t)regs->ebx);
            break;
        case SYS_WAITPID:
            regs->eax = (uint32_t)sys_waitpid((int32_t)regs->ebx,
                                              (int32_t *)regs->ecx,
                                              (int)regs->edx);
            break;
        case SYS_CHDIR:
            regs->eax = (uint32_t)sys_chdir((const char *)regs->ebx);
            break;
        case SYS_GETCWD:
            regs->eax = (uint32_t)sys_getcwd((char *)regs->ebx, regs->ecx);
            break;
        case SYS_STAT:
            regs->eax = (uint32_t)sys_stat((const char *)regs->ebx,
                                           (void *)regs->ecx);
            break;
        case SYS_FSTAT:
            regs->eax = (uint32_t)sys_fstat((int32_t)regs->ebx,
                                            (void *)regs->ecx);
            break;
        case SYS_ALARM:
            regs->eax = (uint32_t)sys_alarm(regs->ebx);
            break;
        case SYS_PAUSE: {
            process_pause();
            regs->eax = (uint32_t)-1;   /* pause always returns -1 (EINTR) */
            break;
        }
        case SYS_GETPPID: {
            process_t *self = process_get_current();
            regs->eax = (uint32_t)(self ? self->parent_pid : 0);
            break;
        }
        case SYS_PIPE:
            regs->eax = (uint32_t)sys_pipe((int32_t *)regs->ebx);
            break;
        case SYS_DUP2:
            regs->eax = (uint32_t)sys_dup2((int32_t)regs->ebx,
                                           (int32_t)regs->ecx);
            break;
        case SYS_DUP:
            regs->eax = (uint32_t)sys_dup((int32_t)regs->ebx);
            break;
        case SYS_GETPROCS:
            regs->eax = (uint32_t)sys_getprocs((void *)regs->ebx,
                                               (int)regs->ecx);
            break;
        case SYS_UPTIME:
            regs->eax = (uint32_t)sys_uptime();
            break;
        case SYS_CPUTIME:
            regs->eax = (uint32_t)sys_cputime();
            break;
        case SYS_SHM:
            regs->eax = (uint32_t)sys_shm();
            break;
        case SYS_SEM_INIT:
            regs->eax = (uint32_t)sys_sem_init((int)regs->ebx, (int)regs->ecx);
            break;
        case SYS_SEM_WAIT:
            regs->eax = (uint32_t)sys_sem_wait((int)regs->ebx);
            break;
        case SYS_SEM_POST:
            regs->eax = (uint32_t)sys_sem_post((int)regs->ebx);
            break;
        case SYS_MMAP:
            regs->eax = (uint32_t)sys_mmap((int)regs->ebx);
            break;
        case SYS_MUNMAP:
            regs->eax = (uint32_t)sys_munmap(regs->ebx, (int)regs->ecx);
            break;
        case SYS_THREAD_CREATE:
            regs->eax = (uint32_t)sys_thread_create(regs->ebx, regs->ecx);
            break;
        case SYS_THREAD_JOIN:
            process_thread_join();
            regs->eax = 0;
            break;
        case SYS_TIME:
            regs->eax = (uint32_t)sys_time((void *)regs->ebx);
            break;
        case SYS_SETENV:
            regs->eax = (uint32_t)sys_setenv((const char *)regs->ebx,
                                             (const char *)regs->ecx);
            break;
        case SYS_GETENV:
            regs->eax = (uint32_t)sys_getenv((const char *)regs->ebx,
                                             (char *)regs->ecx, regs->edx);
            break;
        case SYS_YIELD:
            schedule();
            regs->eax = 0;
            break;
        case SYS_GETPID:
            regs->eax = (uint32_t)process_get_current_pid();
            break;
        case SYS_SLEEP:
            regs->eax = (uint32_t)timer_sleep(regs->ebx);
            break;
        case SYS_CREATE:
            regs->eax = (uint32_t)sys_create((const char *)regs->ebx);
            break;
        case SYS_WRITE_FILE:
            regs->eax = (uint32_t)sys_write_file((int32_t)regs->ebx,
                                                 (const char *)regs->ecx,
                                                 regs->edx);
            break;
        case SYS_UNLINK:
            regs->eax = (uint32_t)sys_unlink((const char *)regs->ebx);
            break;
        case SYS_SEEK:
            regs->eax = (uint32_t)sys_seek((int32_t)regs->ebx,
                                           (int32_t)regs->ecx,
                                           (int32_t)regs->edx);
            break;
        case SYS_MKDIR:
            regs->eax = (uint32_t)sys_mkdir((const char *)regs->ebx);
            break;
        case SYS_RMDIR:
            regs->eax = (uint32_t)sys_rmdir((const char *)regs->ebx);
            break;
        case SYS_READDIR:
            regs->eax = (uint32_t)sys_readdir((const char *)regs->ebx,
                                              regs->ecx,
                                              (char *)regs->edx);
            break;
        case SYS_SPAWN_ARGV: {
            /* ebx = user char **argv, ecx = argc */
            const char **user_argv = (const char **)regs->ebx;
            int argc = (int)regs->ecx;
            char bufs[MAX_SPAWN_ARGS][MAX_USER_STRING + 1];
            const char *argv[MAX_SPAWN_ARGS];
            int ok = 1;
            int i;

            if (argc <= 0 || argc > MAX_SPAWN_ARGS ||
                !user_buffer_valid(user_argv,
                                   (uint32_t)argc * sizeof(char *))) {
                regs->eax = (uint32_t)-1;
                break;
            }
            for (i = 0; i < argc; i++) {
                if (!user_string_valid(user_argv[i])) { ok = 0; break; }
                strcpy(bufs[i], user_argv[i]);
                argv[i] = bufs[i];
            }
            regs->eax = ok ? (uint32_t)elf_spawn_argv(argc, argv)
                           : (uint32_t)-1;
            break;
        }
        case SYS_SBRK:
            regs->eax = (uint32_t)sys_sbrk((int32_t)regs->ebx);
            break;
        case SYS_SIGNAL:
            regs->eax = (uint32_t)sys_signal((int)regs->ebx,
                                             regs->ecx, regs->edx);
            break;
        case SYS_SIGRETURN:
            sys_sigreturn(regs);
            return;  /* context fully restored; do not run signal_deliver */
        case SYS_KILL:
            regs->eax = (uint32_t)sys_kill((int)regs->ebx, (int)regs->ecx);
            break;
        case SYS_FORK:
            regs->eax = (uint32_t)sys_fork(regs);
            break;
        case SYS_EXECV:
            /* On success sys_execv has already redirected regs to the new
             * program; only report failure. */
            if (sys_execv(regs, (int)regs->ecx,
                          (const char **)regs->ebx) != 0) {
                regs->eax = (uint32_t)-1;
            }
            break;
        case SYS_EXEC_ARGV: {
            /* Like SYS_SPAWN_ARGV but blocks until the child exits.
             * Returns the child's exit status (or -1 on exec failure). */
            const char **user_argv = (const char **)regs->ebx;
            int argc = (int)regs->ecx;
            char bufs[MAX_SPAWN_ARGS][MAX_USER_STRING + 1];
            const char *argv[MAX_SPAWN_ARGS];
            int ok = 1;
            int i;
            int32_t pid;

            if (argc <= 0 || argc > MAX_SPAWN_ARGS ||
                !user_buffer_valid(user_argv,
                                   (uint32_t)argc * sizeof(char *))) {
                regs->eax = (uint32_t)-1;
                break;
            }
            for (i = 0; i < argc; i++) {
                if (!user_string_valid(user_argv[i])) { ok = 0; break; }
                strcpy(bufs[i], user_argv[i]);
                argv[i] = bufs[i];
            }
            if (!ok) { regs->eax = (uint32_t)-1; break; }
            pid = elf_spawn_argv(argc, argv);
            if (pid < 0) { regs->eax = (uint32_t)-1; break; }
            regs->eax = (uint32_t)process_wait(pid);
            break;
        }
        default:
            regs->eax = (uint32_t)-1;
            break;
    }

    /* Deliver any signal that became pending during this syscall. */
    signal_deliver(regs);
}

void syscall_install(void) {
    register_interrupt_handler(0x80, syscall_handler);
}
