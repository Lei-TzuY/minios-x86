#include "vga.h"
#include "gdt.h"
#include "idt.h"
#include "isr.h"
#include "kb.h"
#include "timer.h"
#include "multiboot.h"
#include "pmm.h"
#include "paging.h"
#include "process.h"
#include "pipe.h"
#include "syscall.h"
#include "task.h"
#include "heap.h"
#include "utils.h"
#include "fs.h"
#include "ramfs.h"
#include "elf_loader.h"
#include "ata.h"
#include "diskfs.h"
#include "fat16.h"
#include "procfs.h"
#include "rtc.h"

#define SHELL_CMD_SIZE 128
#define SHELL_MAX_ARGS 8
#define SHELL_MAX_STAGES 4

/* Shell working directory; children inherit it, relative paths resolve to it. */
static char shell_cwd[PROCESS_CWD_MAX] = "/";

extern uint8_t kernel_end;

/* Provided by hello_embed.c (auto-generated from user/hello.elf) */
extern const uint8_t  hello_elf_data[];
extern const uint32_t hello_elf_size;
extern const uint8_t  cat_elf_data[];
extern const uint32_t cat_elf_size;
extern const uint8_t  fault_elf_data[];
extern const uint32_t fault_elf_size;
extern const uint8_t  badptr_elf_data[];
extern const uint32_t badptr_elf_size;
extern const uint8_t  worker_elf_data[];
extern const uint32_t worker_elf_size;
extern const uint8_t  spawner_elf_data[];
extern const uint32_t spawner_elf_size;
extern const uint8_t  orphan_elf_data[];
extern const uint32_t orphan_elf_size;
extern const uint8_t  sleeptest_elf_data[];
extern const uint32_t sleeptest_elf_size;
extern const uint8_t  fstest_elf_data[];
extern const uint32_t fstest_elf_size;
extern const uint8_t  echo_elf_data[];
extern const uint32_t echo_elf_size;
extern const uint8_t  malloctest_elf_data[];
extern const uint32_t malloctest_elf_size;
extern const uint8_t  wc_elf_data[];
extern const uint32_t wc_elf_size;
extern const uint8_t  grep_elf_data[];
extern const uint32_t grep_elf_size;
extern const uint8_t  head_elf_data[];
extern const uint32_t head_elf_size;
extern const uint8_t  tail_elf_data[];
extern const uint32_t tail_elf_size;
extern const uint8_t  sort_elf_data[];
extern const uint32_t sort_elf_size;
extern const uint8_t  sigtest_elf_data[];
extern const uint32_t sigtest_elf_size;
extern const uint8_t  sigipc_elf_data[];
extern const uint32_t sigipc_elf_size;
extern const uint8_t  forktest_elf_data[];
extern const uint32_t forktest_elf_size;
extern const uint8_t  execdemo_elf_data[];
extern const uint32_t execdemo_elf_size;
extern const uint8_t  demandtest_elf_data[];
extern const uint32_t demandtest_elf_size;
extern const uint8_t  sigchld_elf_data[];
extern const uint32_t sigchld_elf_size;
extern const uint8_t  waitdemo_elf_data[];
extern const uint32_t waitdemo_elf_size;
extern const uint8_t  cwddemo_elf_data[];
extern const uint32_t cwddemo_elf_size;
extern const uint8_t  statdemo_elf_data[];
extern const uint32_t statdemo_elf_size;
extern const uint8_t  cowstress_elf_data[];
extern const uint32_t cowstress_elf_size;
extern const uint8_t  alarmdemo_elf_data[];
extern const uint32_t alarmdemo_elf_size;
extern const uint8_t  pausedemo_elf_data[];
extern const uint32_t pausedemo_elf_size;
extern const uint8_t  pipedemo_elf_data[];
extern const uint32_t pipedemo_elf_size;
extern const uint8_t  jobctl_elf_data[];
extern const uint32_t jobctl_elf_size;
extern const uint8_t  uptime_elf_data[];
extern const uint32_t uptime_elf_size;
extern const uint8_t  date_elf_data[];
extern const uint32_t date_elf_size;
extern const uint8_t  printenv_elf_data[];
extern const uint32_t printenv_elf_size;
extern const uint8_t  cputime_elf_data[];
extern const uint32_t cputime_elf_size;
extern const uint8_t  shmtest_elf_data[];
extern const uint32_t shmtest_elf_size;
extern const uint8_t  semtest_elf_data[];
extern const uint32_t semtest_elf_size;
extern const uint8_t  mmaptest_elf_data[];
extern const uint32_t mmaptest_elf_size;
extern const uint8_t  threadtest_elf_data[];
extern const uint32_t threadtest_elf_size;
extern const uint8_t  threadexit_elf_data[];
extern const uint32_t threadexit_elf_size;
extern const uint8_t  execguard_elf_data[];
extern const uint32_t execguard_elf_size;
extern const uint8_t  ramgrow_elf_data[];
extern const uint32_t ramgrow_elf_size;
extern const uint8_t  pathlim_elf_data[];
extern const uint32_t pathlim_elf_size;
extern const uint8_t  redirref_elf_data[];
extern const uint32_t redirref_elf_size;
extern const uint8_t  fatref_elf_data[];
extern const uint32_t fatref_elf_size;
extern const uint8_t  forkredir_elf_data[];
extern const uint32_t forkredir_elf_size;
extern const uint8_t  sigretguard_elf_data[];
extern const uint32_t sigretguard_elf_size;
extern const uint8_t  killthread_elf_data[];
extern const uint32_t killthread_elf_size;
extern const uint8_t  killwait_elf_data[];
extern const uint32_t killwait_elf_size;
extern const uint8_t  sigflags_elf_data[];
extern const uint32_t sigflags_elf_size;
extern const uint8_t  fatgrow_elf_data[];
extern const uint32_t fatgrow_elf_size;
extern const uint8_t  bigseek_elf_data[];
extern const uint32_t bigseek_elf_size;
extern const uint8_t  stress_elf_data[];
extern const uint32_t stress_elf_size;
extern const uint8_t  ush_elf_data[];
extern const uint32_t ush_elf_size;
extern const uint8_t  fat16_image_data[];
extern const uint32_t fat16_image_size;

static const uint8_t readme_text[] =
    "miniOS RAMFS file access works.\n"
    "This text was read by a ring 3 program through file syscalls.\n";

static void free_usable_subrange(uint64_t base, uint64_t length,
                                 uint32_t allowed_start,
                                 uint32_t allowed_end) {
    uint64_t end = base + length;
    uint64_t start;

    if (end < base) end = (uint64_t)-1;
    start = base > allowed_start ? base : allowed_start;
    if (end > allowed_end) end = allowed_end;

    if (end > start) {
        pmm_init_region((uint32_t)start, (uint32_t)(end - start));
    }
}

static void free_usable_memory(uint64_t base, uint64_t length,
                               uint32_t free_start) {
    free_usable_subrange(base, length, free_start, USER_LOAD_BASE);
    free_usable_subrange(base, length, 0x400000U, PMM_MAX_MEMORY);
}

static void initialize_physical_memory(uint32_t magic,
                                       const multiboot_info_t *info,
                                       uint32_t free_start) {
    uint32_t memory_kb = PMM_MAX_MEMORY / 1024;

    if (magic == MULTIBOOT_BOOTLOADER_MAGIC && info &&
        (info->flags & MULTIBOOT_INFO_MEMORY)) {
        uint64_t detected_kb = 1024U + (uint64_t)info->mem_upper;
        if (detected_kb < memory_kb) memory_kb = (uint32_t)detected_kb;
    }

    pmm_init(memory_kb);

    if (magic == MULTIBOOT_BOOTLOADER_MAGIC && info &&
        (info->flags & MULTIBOOT_INFO_MEM_MAP)) {
        uint32_t offset = 0;

        while (offset < info->mmap_length) {
            multiboot_mmap_entry_t *entry =
                (multiboot_mmap_entry_t *)(info->mmap_addr + offset);
            uint32_t record_size = entry->size + sizeof(entry->size);

            if (entry->size < sizeof(*entry) - sizeof(entry->size) ||
                record_size > info->mmap_length - offset) {
                break;
            }
            if (entry->type == MULTIBOOT_MEMORY_AVAILABLE) {
                free_usable_memory(entry->addr, entry->len, free_start);
            }
            offset += record_size;
        }
        terminal_writestring("Initialized PMM from Multiboot memory map.\n");
    } else {
        free_usable_memory(0, (uint64_t)memory_kb * 1024U, free_start);
        terminal_writestring("Initialized PMM from fallback memory size.\n");
    }
}

/* keyboard_read returns however many chars are immediately available.
 * The shell needs true line input: block until the user presses Enter. */
static size_t shell_readline(char *buf, size_t size) {
    size_t n = 0;
    while (n + 1 < size) {
        char c;
        keyboard_read(&c, 1);   /* blocks until 1 char available */
        if (c == '\n' || c == '\r') break;
        if (c == '\b') {        /* backspace */
            if (n > 0) n--;
        } else if ((unsigned char)c >= 0x20) {
            buf[n++] = c;
        }
    }
    buf[n] = '\0';
    return n;
}

static void shell_print_help(void) {
    terminal_writestring("Built-in commands:\n");
    terminal_writestring("  help   Show this help\n");
    terminal_writestring("  ls     List files in RAMFS\n");
    terminal_writestring("  clear  Clear the screen\n");
    terminal_writestring("  mem    Show memory allocator statistics\n");
    terminal_writestring("  heaptest  Exercise multi-page heap allocation\n");
    terminal_writestring("  pmtest  Exercise high physical memory allocation\n");
    terminal_writestring("  atatest  Exercise ATA PIO sector I/O\n");
    terminal_writestring("  disktest  Exercise persistent disk filesystem\n");
    terminal_writestring("  ps     List running processes\n");
    terminal_writestring("  kill <pid> [sig]  Signal a process (default: kill)\n");
    terminal_writestring("  pwd    Print the working directory\n");
    terminal_writestring("  cd [dir]  Change the working directory\n");
    terminal_writestring("  rm <file...>  Remove files\n");
    terminal_writestring("  mkdir/rmdir <dir>  Create/remove a directory\n");
    terminal_writestring("  touch <file>  Create an empty file\n");
    terminal_writestring("  cp/mv <src> <dst>  Copy/move a file\n");
    terminal_writestring("Run a RAMFS program by typing its file name.\n");
    terminal_writestring("Append '&' to run a program in the background.\n");
}

static const char *process_state_name(process_state_t state) {
    switch (state) {
        case PROCESS_RUNNING: return "running";
        case PROCESS_ZOMBIE:  return "zombie";
        default:              return "unused";
    }
}

static void shell_ps(void) {
    process_info_t info[MAX_PROCESSES];
    uint32_t count = process_snapshot(info, MAX_PROCESSES);

    terminal_writestring("  PID  PPID  STATE    NAME\n");
    for (uint32_t i = 0; i < count; i++) {
        terminal_writestring("  ");
        terminal_write_dec((uint32_t)info[i].pid);
        terminal_writestring("    ");
        terminal_write_dec((uint32_t)info[i].parent_pid);
        terminal_writestring("    ");
        terminal_writestring(process_state_name(info[i].state));
        terminal_writestring("  ");
        terminal_writestring(info[i].name);
        terminal_putchar('\n');
    }
    terminal_writestring("  (");
    terminal_write_dec(count);
    terminal_writestring(" processes)\n");
}

/* Parse a non-negative decimal integer; returns -1 on any invalid character. */
static int32_t shell_parse_pid(const char *s) {
    int32_t value = 0;

    if (!s || !*s) return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        value = value * 10 + (*s - '0');
        if (value > 0x7FFFFFF) return -1;
    }
    return value;
}

/* kill <pid> [sig]: default force-kills (SIGKILL); a signal number sends that
 * signal instead (catchable if the target installed a handler). */
static void shell_kill(const char *pid_str, const char *sig_str) {
    int32_t pid = shell_parse_pid(pid_str);
    int32_t sig = sig_str ? shell_parse_pid(sig_str) : SIGKILL;
    int result;

    if (pid < 0 || sig < 0) {
        terminal_writestring("kill: invalid argument\n");
        return;
    }

    result = (sig == SIGKILL) ? process_kill(pid)
                              : process_send_signal(pid, (int)sig);
    if (result != 0) {
        terminal_writestring("kill: no such process\n");
        return;
    }
    terminal_writestring("kill: signalled pid ");
    terminal_write_dec((uint32_t)pid);
    terminal_putchar('\n');
}

static void shell_list_files(const char *path) {
    char abs[FS_MAX_PATH];
    fs_node_t *dir;
    uint32_t index = 0;
    dirent_t *entry;

    if (vfs_resolve_path(shell_cwd, path ? path : ".", abs) != 0) {
        terminal_writestring("ls: invalid path\n");
        return;
    }
    dir = resolve_fs(abs);
    if (!dir || dir->flags != FS_DIRECTORY) {
        terminal_writestring("ls: not found: ");
        terminal_writestring(path ? path : abs);
        terminal_putchar('\n');
        return;
    }
    while ((entry = readdir_fs(dir, index++)) != NULL) {
        terminal_writestring(entry->name);
        terminal_putchar('\n');
    }
}

static void shell_print_memory(void) {
    terminal_writestring("PMM blocks: total=");
    terminal_write_dec(pmm_get_total_blocks());
    terminal_writestring(" used=");
    terminal_write_dec(pmm_get_used_blocks());
    terminal_writestring(" free=");
    terminal_write_dec(pmm_get_free_blocks());
    terminal_writestring("\nKernel heap: pages=");
    terminal_write_dec(heap_get_page_count());
    terminal_writestring(" free-bytes=");
    terminal_write_dec(heap_get_free_bytes());
    terminal_writestring("\nUser pages: accessible=");
    terminal_write_dec(paging_get_user_page_count());
    terminal_writestring(" spaces=");
    terminal_write_dec(paging_get_user_space_count());
    terminal_writestring("\nProcesses: running=");
    terminal_write_dec(process_get_running_count());
    terminal_writestring(" zombies=");
    terminal_write_dec(process_get_zombie_count());
    terminal_writestring(" peak=");
    terminal_write_dec(process_get_peak_count());
    terminal_writestring("\nTasks: blocked=");
    terminal_write_dec(task_get_blocked_count());
    terminal_writestring("\nTimers: sleeping=");
    terminal_write_dec(timer_get_sleeping_count());
    terminal_writestring("\nRAMFS nodes=");
    terminal_write_dec(ramfs_get_node_count());
    terminal_writestring("\nATA sectors: available=");
    terminal_write_dec(ata_get_sector_count());
    terminal_writestring(" reads=");
    terminal_write_dec(ata_get_read_count());
    terminal_writestring(" writes=");
    terminal_write_dec(ata_get_write_count());
    terminal_writestring("\nDiskFS: mounted=");
    terminal_write_dec(diskfs_is_mounted());
    terminal_writestring(" generation=");
    terminal_write_dec(diskfs_get_generation());
    terminal_writestring(" files=");
    terminal_write_dec(diskfs_get_file_count());
    terminal_putchar('\n');
}

static void shell_heap_test(void) {
    static const size_t sizes[] = { 64, 2048, PMM_BLOCK_SIZE + 512, 96 };
    void *blocks[sizeof(sizes) / sizeof(sizes[0])] = { 0 };
    int passed = 1;

    for (size_t i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
        uint8_t pattern = (uint8_t)(0x30 + i);
        blocks[i] = kmalloc(sizes[i]);
        if (!blocks[i]) {
            passed = 0;
            break;
        }
        memset(blocks[i], pattern, sizes[i]);
        for (size_t j = 0; j < sizes[i]; j++) {
            if (((uint8_t *)blocks[i])[j] != pattern) {
                passed = 0;
                break;
            }
        }
        if (!passed) break;
    }

    for (size_t i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
        kfree(blocks[i]);
    }

    if (passed)
        terminal_writestring("[heap test passed]\n");
    else
        terminal_writestring("[heap test failed]\n");
}

static void shell_pmm_test(void) {
    const uint32_t block_count = 512;
    uint8_t *memory = (uint8_t *)pmm_alloc_blocks(block_count);
    int passed = memory != NULL;

    if (memory) {
        for (uint32_t i = 0; i < block_count; i++) {
            memory[i * PMM_BLOCK_SIZE] = (uint8_t)i;
        }
        for (uint32_t i = 0; i < block_count; i++) {
            if (memory[i * PMM_BLOCK_SIZE] != (uint8_t)i) {
                passed = 0;
                break;
            }
        }
        pmm_free_blocks(memory, block_count);
    }

    if (passed)
        terminal_writestring("[pmm high-memory test passed]\n");
    else
        terminal_writestring("[pmm high-memory test failed]\n");
}

static void shell_ata_test(void) {
    static const uint8_t expected[] = "MINIOS ATA PIO READ TEST\n";
    uint8_t sector[ATA_SECTOR_SIZE];
    uint8_t written[ATA_SECTOR_SIZE];
    int passed = ata_is_available() && ata_read_sector(1, sector);

    if (passed) {
        for (size_t i = 0; i < sizeof(expected) - 1; i++) {
            if (sector[i] != expected[i]) {
                passed = 0;
                break;
            }
        }
    }

    if (passed && ata_read_sector(ata_get_sector_count(), sector)) {
        passed = 0;
    }

    for (size_t i = 0; i < sizeof(written); i++) {
        written[i] = (uint8_t)(i ^ 0xA5);
    }

    if (passed && (!ata_write_sector(2, written) ||
                   !ata_read_sector(2, sector))) {
        passed = 0;
    }

    if (passed) {
        for (size_t i = 0; i < sizeof(written); i++) {
            if (sector[i] != written[i]) {
                passed = 0;
                break;
            }
        }
    }

    if (passed && ata_write_sector(ata_get_sector_count(), written)) {
        passed = 0;
    }

    if (passed)
        terminal_writestring("[ata pio read/write test passed]\n");
    else
        terminal_writestring("[ata pio read/write test failed]\n");
}

static int buffers_equal(const uint8_t *first, const uint8_t *second,
                         size_t size) {
    for (size_t i = 0; i < size; i++) {
        if (first[i] != second[i]) return 0;
    }
    return 1;
}

static void shell_disk_test(void) {
    static const uint8_t seed[] = "MINIOS DISKFS SEED\n";
    static const uint8_t payload[] = "persistent diskfs write works\n";
    static const uint8_t nested_payload[] = "nested diskfs write works\n";
    uint8_t buffer[64];
    fs_node_t *nested;
    int passed =
        diskfs_is_mounted() &&
        diskfs_get_generation() >= 7 &&
        diskfs_get_file_count() == 1 &&
        diskfs_read_file("seed.txt", buffer, sizeof(buffer)) ==
            (int)(sizeof(seed) - 1) &&
        buffers_equal(buffer, seed, sizeof(seed) - 1) &&
        diskfs_format() &&
        diskfs_get_generation() == 1 &&
        diskfs_get_file_count() == 0 &&
        diskfs_create_file("persist.txt") &&
        !diskfs_create_file("persist.txt") &&
        !diskfs_create_file("bad/name") &&
        diskfs_write_file("persist.txt", payload, sizeof(payload) - 1) &&
        mkdir_fs("/disk/docs") == 0 &&
        (nested = create_fs("/disk/docs/note.txt")) != NULL &&
        write_fs(nested, 0, sizeof(nested_payload) - 1,
                 (uint8_t *)nested_payload) == sizeof(nested_payload) - 1 &&
        diskfs_mount() &&
        diskfs_get_generation() == 6 &&
        diskfs_get_file_count() == 2 &&
        diskfs_read_file("persist.txt", buffer, sizeof(buffer)) ==
            (int)(sizeof(payload) - 1) &&
        buffers_equal(buffer, payload, sizeof(payload) - 1) &&
        (nested = resolve_fs("/disk/docs/note.txt")) != NULL &&
        read_fs(nested, 0, sizeof(buffer), buffer) ==
            sizeof(nested_payload) - 1 &&
        buffers_equal(buffer, nested_payload, sizeof(nested_payload) - 1) &&
        unlink_fs("/disk/docs/note.txt") == 0 &&
        rmdir_fs("/disk/docs") == 0 &&
        diskfs_unlink_file("persist.txt") &&
        diskfs_mount() &&
        diskfs_get_generation() == 9 &&
        diskfs_get_file_count() == 0 &&
        diskfs_read_file("persist.txt", buffer, sizeof(buffer)) < 0;

    if (passed)
        terminal_writestring("[diskfs persistence test passed]\n");
    else
        terminal_writestring("[diskfs persistence test failed]\n");
}

/* Split a mutable line into whitespace-delimited tokens in-place. */
static int shell_parse_args(char *line, const char **argv, int max_args) {
    int argc = 0;
    char *p = line;

    while (*p && argc < max_args) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = '\0';
    }
    return argc;
}

/* Strip redirection tokens (> >> <) and their filenames out of argv, recording
 * the targets. Returns 0 on success, -1 on a syntax error (missing filename). */
static int shell_extract_redirects(const char **argv, int *argc,
                                   const char **out_path, int *append,
                                   const char **in_path) {
    int n = *argc, w = 0;

    *out_path = NULL;
    *in_path = NULL;
    *append = 0;

    for (int i = 0; i < n; i++) {
        if (strcmp(argv[i], ">") == 0 || strcmp(argv[i], ">>") == 0) {
            if (i + 1 >= n) return -1;
            *out_path = argv[i + 1];
            *append = (argv[i][1] == '>');
            i++;
        } else if (strcmp(argv[i], "<") == 0) {
            if (i + 1 >= n) return -1;
            *in_path = argv[i + 1];
            i++;
        } else {
            argv[w++] = argv[i];
        }
    }

    *argc = w;
    return 0;
}

/* Resolve the stdout redirect target (relative to shell_cwd), creating or
 * truncating as needed. Returns the node, or NULL on error (message printed). */
static fs_node_t *shell_open_out(const char *path, int append, uint32_t *offset) {
    char abs[FS_MAX_PATH];
    fs_node_t *node;

    if (vfs_resolve_path(shell_cwd, path, abs) != 0) {
        terminal_writestring("cannot open for writing: ");
        terminal_writestring(path);
        terminal_putchar('\n');
        return NULL;
    }

    if (!append) {
        /* '>' truncates: drop any existing node and make a fresh writable one. */
        unlink_fs(abs);
        node = create_fs(abs);
    } else {
        node = resolve_fs(abs);
        if (!node) node = create_fs(abs);
    }

    if (!node || node->flags != FS_FILE || !node->write) {
        terminal_writestring("cannot open for writing: ");
        terminal_writestring(path);
        terminal_putchar('\n');
        return NULL;
    }

    *offset = append ? node->length : 0;
    return node;
}

/* Resolve a stdin redirect source (relative to shell_cwd). NULL on error. */
static fs_node_t *shell_open_in(const char *path) {
    char abs[FS_MAX_PATH];
    fs_node_t *node;

    if (vfs_resolve_path(shell_cwd, path, abs) != 0 ||
        !(node = resolve_fs(abs)) || node->flags != FS_FILE) {
        terminal_writestring("cannot open for reading: ");
        terminal_writestring(path);
        terminal_putchar('\n');
        return NULL;
    }
    return node;
}

/* Copy file `src` to `dst` (both relative to shell_cwd). Returns 0 on success. */
static int shell_copy(const char *src, const char *dst) {
    char sabs[FS_MAX_PATH], dabs[FS_MAX_PATH];
    fs_node_t *snode, *dnode;
    uint8_t buf[128];
    uint32_t off = 0, n;

    if (vfs_resolve_path(shell_cwd, src, sabs) != 0 ||
        vfs_resolve_path(shell_cwd, dst, dabs) != 0) return -1;
    if (strcmp(sabs, dabs) == 0) return 0;   /* copying onto itself: no-op */

    snode = resolve_fs(sabs);
    if (!snode || snode->flags != FS_FILE) return -1;

    unlink_fs(dabs);                          /* truncate any existing target */
    dnode = create_fs(dabs);
    if (!dnode || !dnode->write) return -1;

    while ((n = read_fs(snode, off, sizeof(buf), buf)) > 0) {
        if (write_fs(dnode, off, n, buf) != n) return -1;
        off += n;
    }
    return 0;
}

/* mkdir/rmdir/touch wrappers that resolve relative to shell_cwd and report
 * errors uniformly. Each returns nothing; messages go to the terminal. */
static void shell_mkdir(const char *path) {
    char abs[FS_MAX_PATH];
    if (vfs_resolve_path(shell_cwd, path, abs) != 0 || mkdir_fs(abs) != 0) {
        terminal_writestring("mkdir: cannot create directory: ");
        terminal_writestring(path);
        terminal_putchar('\n');
    }
}

static void shell_rmdir(const char *path) {
    char abs[FS_MAX_PATH];
    if (vfs_resolve_path(shell_cwd, path, abs) != 0 || rmdir_fs(abs) != 0) {
        terminal_writestring("rmdir: cannot remove directory: ");
        terminal_writestring(path);
        terminal_putchar('\n');
    }
}

static void shell_touch(const char *path) {
    char abs[FS_MAX_PATH];
    fs_node_t *node;

    if (vfs_resolve_path(shell_cwd, path, abs) != 0) {
        terminal_writestring("touch: invalid path\n");
        return;
    }
    node = resolve_fs(abs);
    if (!node && !create_fs(abs)) {           /* create only if absent */
        terminal_writestring("touch: cannot create: ");
        terminal_writestring(path);
        terminal_putchar('\n');
    }
}

/* Run an N-stage pipeline (cmd0 | cmd1 | ... | cmdN-1). Stage i's stdout feeds
 * stage i+1's stdin through an in-kernel pipe; all stages run concurrently.
 * argv still contains the '|' separators, which this function splits on. */
static void shell_run_pipeline(const char **argv, int argc, int background) {
    const char *stage_argv[SHELL_MAX_STAGES][SHELL_MAX_ARGS];
    int     stage_argc[SHELL_MAX_STAGES];
    pipe_t *pipes[SHELL_MAX_STAGES - 1];
    int32_t pids[SHELL_MAX_STAGES];
    int nstages = 1;
    int spawned = 0;
    int32_t status = 0;
    fs_node_t *in_node = NULL, *out_node = NULL;  /* first stdin / last stdout */
    uint32_t out_off = 0;

    /* Split argv into stages on '|'. */
    stage_argc[0] = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "|") == 0) {
            if (stage_argc[nstages - 1] == 0) {
                terminal_writestring("syntax error near '|'\n");
                return;
            }
            if (nstages >= SHELL_MAX_STAGES) {
                terminal_writestring("pipeline too long\n");
                return;
            }
            stage_argc[nstages++] = 0;
        } else {
            stage_argv[nstages - 1][stage_argc[nstages - 1]++] = argv[i];
        }
    }
    if (stage_argc[nstages - 1] == 0) {
        terminal_writestring("syntax error near '|'\n");
        return;
    }

    /* Strip redirects from each stage; honor '<' on the first stage's stdin
     * and '>'/'>>' on the last stage's stdout (other stages use the pipes). */
    for (int s = 0; s < nstages; s++) {
        const char *op, *ip;
        int ap;
        if (shell_extract_redirects(stage_argv[s], &stage_argc[s],
                                    &op, &ap, &ip) != 0 || stage_argc[s] == 0) {
            terminal_writestring("syntax error in pipeline\n");
            return;
        }
        if (s == 0 && ip) {
            in_node = shell_open_in(ip);
            if (!in_node) return;
        }
        if (s == nstages - 1 && op) {
            out_node = shell_open_out(op, ap, &out_off);
            if (!out_node) return;
        }
    }

    /* Create the N-1 connecting pipes. */
    for (int i = 0; i < nstages - 1; i++) {
        pipes[i] = pipe_create();
        if (!pipes[i]) {
            for (int j = 0; j < i; j++) {
                pipe_close_read(pipes[j]);
                pipe_close_write(pipes[j]);
            }
            terminal_writestring("pipe: out of memory\n");
            return;
        }
    }

    /* Spawn each stage and wire its stdin/stdout to the adjacent pipes. */
    for (int i = 0; i < nstages; i++) {
        pids[i] = elf_spawn_argv(stage_argc[i], stage_argv[i]);
        if (pids[i] < 0) break;
        process_set_cwd(pids[i], shell_cwd);
        process_pipe(pids[i],
                     i < nstages - 1 ? pipes[i] : NULL,
                     i > 0 ? pipes[i - 1] : NULL);
        /* First stage may read a file; last stage may write one. */
        if (i == 0 && in_node) process_redirect(pids[i], NULL, 0, in_node);
        if (i == nstages - 1 && out_node)
            process_redirect(pids[i], out_node, out_off, NULL);
        spawned++;
    }

    if (spawned < nstages) {
        /* A stage failed: close the ends no live process owns, then reap the
         * stages already spawned (a broken-pipe cascade makes them exit). */
        if (spawned >= 1) pipe_close_read(pipes[spawned - 1]);
        for (int i = spawned; i < nstages - 1; i++) {
            pipe_close_read(pipes[i]);
            pipe_close_write(pipes[i]);
        }
        for (int i = 0; i < spawned; i++) process_wait(pids[i]);
        return;
    }

    if (background) {
        for (int i = 0; i < nstages; i++) process_detach(pids[i]);
        terminal_setcolor(8);
        terminal_writestring("[pipeline backgrounded]\n");
        terminal_setcolor(15);
        return;
    }

    /* Foreground: Ctrl+C targets the last stage; wait for every stage. */
    keyboard_set_foreground(pids[nstages - 1]);
    for (int i = 0; i < nstages; i++) status = process_wait(pids[i]);
    keyboard_clear_foreground();
    terminal_setcolor(8);
    terminal_writestring(status == -130 ? "[killed]\n" : "[program exited]\n");
    terminal_setcolor(15);
}

static void kernel_shell(void) {
    char line[SHELL_CMD_SIZE];
    const char *argv[SHELL_MAX_ARGS];
    int argc;

    terminal_setcolor(10); /* Bright green */
    terminal_writestring("\n=== miniOS shell ===\n");
    terminal_writestring("Type 'help' for available commands.\n");
    terminal_setcolor(15);
    terminal_writestring("> ");

    while (1) {
        size_t n = shell_readline(line, sizeof(line));

        if (n == 0) {
            terminal_writestring("> ");
            continue;
        }

        argc = shell_parse_args(line, argv, SHELL_MAX_ARGS);
        if (argc == 0) {
            terminal_writestring("> ");
            continue;
        }

        /* A trailing '&' token runs the program in the background. */
        int background = 0;
        if (argc > 1 && strcmp(argv[argc - 1], "&") == 0) {
            background = 1;
            argc--;
        }

        /* A '|' token turns the line into a multi-stage pipeline. */
        int has_pipe = 0;
        for (int i = 0; i < argc; i++) {
            if (strcmp(argv[i], "|") == 0) { has_pipe = 1; break; }
        }
        if (has_pipe) {
            shell_run_pipeline(argv, argc, background);
            terminal_writestring("> ");
            continue;
        }

        /* Pull out I/O redirection (> >> <); applies to user programs. */
        const char *out_path, *in_path;
        int append;
        if (shell_extract_redirects(argv, &argc, &out_path,
                                    &append, &in_path) != 0) {
            terminal_writestring("syntax error: redirection needs a filename\n");
            terminal_writestring("> ");
            continue;
        }
        if (argc == 0) {
            terminal_writestring("> ");
            continue;
        }

        if (strcmp(argv[0], "help") == 0) {
            shell_print_help();
        } else if (strcmp(argv[0], "ls") == 0) {
            shell_list_files(argc > 1 ? argv[1] : NULL);
        } else if (strcmp(argv[0], "clear") == 0) {
            terminal_clear();
        } else if (strcmp(argv[0], "mem") == 0) {
            shell_print_memory();
        } else if (strcmp(argv[0], "uptime") == 0) {
            uint32_t ticks = timer_get_ticks();
            terminal_writestring("up ");
            terminal_write_dec(ticks / 100);
            terminal_writestring("s (ticks=");
            terminal_write_dec(ticks);
            terminal_writestring(")\n");
        } else if (strcmp(argv[0], "date") == 0) {
            rtc_time_t now;
            rtc_read(&now);
            terminal_write_dec_pad(now.year, 4);
            terminal_putchar('-');
            terminal_write_dec_pad(now.month, 2);
            terminal_putchar('-');
            terminal_write_dec_pad(now.day, 2);
            terminal_putchar(' ');
            terminal_write_dec_pad(now.hour, 2);
            terminal_putchar(':');
            terminal_write_dec_pad(now.minute, 2);
            terminal_putchar(':');
            terminal_write_dec_pad(now.second, 2);
            terminal_putchar('\n');
        } else if (strcmp(argv[0], "ps") == 0) {
            shell_ps();
        } else if (strcmp(argv[0], "kill") == 0) {
            shell_kill(argc > 1 ? argv[1] : NULL, argc > 2 ? argv[2] : NULL);
        } else if (strcmp(argv[0], "pwd") == 0) {
            terminal_writestring(shell_cwd);
            terminal_putchar('\n');
        } else if (strcmp(argv[0], "cd") == 0) {
            char abs[FS_MAX_PATH];
            const char *target = argc > 1 ? argv[1] : "/";
            fs_node_t *dir;

            if (vfs_resolve_path(shell_cwd, target, abs) != 0) {
                terminal_writestring("cd: invalid path\n");
            } else if (!(dir = resolve_fs(abs)) || dir->flags != FS_DIRECTORY) {
                terminal_writestring("cd: not a directory: ");
                terminal_writestring(target);
                terminal_putchar('\n');
            } else {
                strcpy(shell_cwd, abs);
            }
        } else if (strcmp(argv[0], "rm") == 0) {
            char abs[FS_MAX_PATH];
            if (argc < 2) {
                terminal_writestring("rm: missing operand\n");
            }
            for (int a = 1; a < argc; a++) {
                if (vfs_resolve_path(shell_cwd, argv[a], abs) != 0 ||
                    unlink_fs(abs) != 0) {
                    terminal_writestring("rm: cannot remove: ");
                    terminal_writestring(argv[a]);
                    terminal_putchar('\n');
                }
            }
        } else if (strcmp(argv[0], "mkdir") == 0) {
            if (argc < 2) terminal_writestring("mkdir: missing operand\n");
            else shell_mkdir(argv[1]);
        } else if (strcmp(argv[0], "rmdir") == 0) {
            if (argc < 2) terminal_writestring("rmdir: missing operand\n");
            else shell_rmdir(argv[1]);
        } else if (strcmp(argv[0], "touch") == 0) {
            if (argc < 2) terminal_writestring("touch: missing operand\n");
            else shell_touch(argv[1]);
        } else if (strcmp(argv[0], "cp") == 0) {
            if (argc < 3) terminal_writestring("cp: usage: cp <src> <dst>\n");
            else if (shell_copy(argv[1], argv[2]) != 0)
                terminal_writestring("cp: failed\n");
        } else if (strcmp(argv[0], "mv") == 0) {
            if (argc < 3) {
                terminal_writestring("mv: usage: mv <src> <dst>\n");
            } else if (shell_copy(argv[1], argv[2]) != 0) {
                terminal_writestring("mv: failed\n");
            } else {
                char sabs[FS_MAX_PATH];
                if (vfs_resolve_path(shell_cwd, argv[1], sabs) == 0)
                    unlink_fs(sabs);
            }
        } else if (strcmp(argv[0], "heaptest") == 0) {
            shell_heap_test();
        } else if (strcmp(argv[0], "pmtest") == 0) {
            shell_pmm_test();
        } else if (strcmp(argv[0], "atatest") == 0) {
            shell_ata_test();
        } else if (strcmp(argv[0], "disktest") == 0) {
            shell_disk_test();
        } else {
            fs_node_t *out_node = NULL, *in_node = NULL;
            uint32_t out_off = 0;
            int redir_ok = 1;

            if (out_path) {
                out_node = shell_open_out(out_path, append, &out_off);
                if (!out_node) redir_ok = 0;
            }
            if (redir_ok && in_path) {
                in_node = shell_open_in(in_path);
                if (!in_node) redir_ok = 0;
            }

            int32_t pid = redir_ok ? elf_spawn_argv(argc, argv) : -1;
            if (pid >= 0) {
                process_set_cwd(pid, shell_cwd);
                process_redirect(pid, out_node, out_off, in_node);
            }

            if (pid >= 0 && background) {
                /* Detached: self-releases on exit; shell does not wait. */
                process_detach(pid);
                terminal_setcolor(8);
                terminal_writestring("[pid ");
                terminal_write_dec((uint32_t)pid);
                terminal_writestring("]\n");
                terminal_setcolor(15);
            } else if (pid >= 0) {
                int32_t status;
                keyboard_set_foreground(pid);
                status = process_wait(pid);
                keyboard_clear_foreground();
                terminal_setcolor(8);
                terminal_writestring(status == -130 ? "[killed]\n"
                                                    : "[program exited]\n");
                terminal_setcolor(15);
            }
        }

        terminal_writestring("> ");
    }
}

void kernel_main(uint32_t multiboot_magic, const multiboot_info_t *multiboot) {
    terminal_initialize();
    terminal_writestring("Booting Advanced OS...\n");

    /* Core System */
    gdt_install();
    set_kernel_stack(0x90000);
    idt_install();
    isr_install();
    syscall_install();

    /* Memory Management:
     * Give PMM the range free_start..0x300000 for kernel heap.
     * 0x300000..0x400000 is reserved for user program code/data/stack
     * and is kept out of PMM so kmalloc never touches it. */
    uint32_t free_start = ((uint32_t)&kernel_end + PMM_BLOCK_SIZE - 1)
                          & ~(PMM_BLOCK_SIZE - 1);
    initialize_physical_memory(multiboot_magic, multiboot, free_start);
    paging_install();
    heap_init();
    tasking_init();

    /* Device Drivers */
    keyboard_install();
    timer_install();
    ata_install();
    if (ata_is_available()) {
        terminal_writestring("ATA primary master sectors: ");
        terminal_write_dec(ata_get_sector_count());
        terminal_putchar('\n');
    } else {
        terminal_writestring("ATA primary master not detected.\n");
    }
    diskfs_install();
    if (diskfs_is_mounted()) {
        terminal_writestring("Mounted DiskFS generation: ");
        terminal_write_dec(diskfs_get_generation());
        terminal_putchar('\n');
    } else {
        terminal_writestring("DiskFS not mounted.\n");
    }

    /* VFS + RAMFS */
    terminal_setcolor(2);
    terminal_writestring("Initializing VFS and RAMFS...\n");
    terminal_setcolor(15);
    ramfs_init();
    if (diskfs_is_mounted() &&
        ramfs_mount("disk", diskfs_get_root_node()) == 0) {
        terminal_writestring("Mounted DiskFS at /disk.\n");
    }

    fat16_install(fat16_image_data, fat16_image_size);
    if (fat16_is_mounted() &&
        ramfs_mount("fat", fat16_get_root_node()) == 0) {
        terminal_writestring("Mounted FAT16 at /fat.\n");
    }

    if (ramfs_mount("proc", procfs_get_root()) == 0) {
        terminal_writestring("Mounted procfs at /proc.\n");
    }

    /* Register the embedded ELF as a read-only RAMFS node (zero-copy:
     * node->ptr points directly into the kernel .rodata array). */
    ramfs_create_static_file("hello", hello_elf_data, hello_elf_size);
    ramfs_create_static_file("cat", cat_elf_data, cat_elf_size);
    ramfs_create_static_file("fault", fault_elf_data, fault_elf_size);
    ramfs_create_static_file("badptr", badptr_elf_data, badptr_elf_size);
    ramfs_create_static_file("worker", worker_elf_data, worker_elf_size);
    ramfs_create_static_file("spawner", spawner_elf_data, spawner_elf_size);
    ramfs_create_static_file("orphan", orphan_elf_data, orphan_elf_size);
    ramfs_create_static_file("sleeptest", sleeptest_elf_data, sleeptest_elf_size);
    ramfs_create_static_file("fstest", fstest_elf_data, fstest_elf_size);
    ramfs_create_static_file("echo", echo_elf_data, echo_elf_size);
    ramfs_create_static_file("malloctest", malloctest_elf_data, malloctest_elf_size);
    ramfs_create_static_file("wc", wc_elf_data, wc_elf_size);
    ramfs_create_static_file("grep", grep_elf_data, grep_elf_size);
    ramfs_create_static_file("head", head_elf_data, head_elf_size);
    ramfs_create_static_file("tail", tail_elf_data, tail_elf_size);
    ramfs_create_static_file("sort", sort_elf_data, sort_elf_size);
    ramfs_create_static_file("sigtest", sigtest_elf_data, sigtest_elf_size);
    ramfs_create_static_file("sigipc", sigipc_elf_data, sigipc_elf_size);
    ramfs_create_static_file("forktest", forktest_elf_data, forktest_elf_size);
    ramfs_create_static_file("execdemo", execdemo_elf_data, execdemo_elf_size);
    ramfs_create_static_file("demandtest", demandtest_elf_data, demandtest_elf_size);
    ramfs_create_static_file("sigchld", sigchld_elf_data, sigchld_elf_size);
    ramfs_create_static_file("waitdemo", waitdemo_elf_data, waitdemo_elf_size);
    ramfs_create_static_file("cwddemo", cwddemo_elf_data, cwddemo_elf_size);
    ramfs_create_static_file("statdemo", statdemo_elf_data, statdemo_elf_size);
    ramfs_create_static_file("cowstress", cowstress_elf_data, cowstress_elf_size);
    ramfs_create_static_file("alarmdemo", alarmdemo_elf_data, alarmdemo_elf_size);
    ramfs_create_static_file("pausedemo", pausedemo_elf_data, pausedemo_elf_size);
    ramfs_create_static_file("pipedemo", pipedemo_elf_data, pipedemo_elf_size);
    ramfs_create_static_file("jobctl", jobctl_elf_data, jobctl_elf_size);
    ramfs_create_static_file("uptime", uptime_elf_data, uptime_elf_size);
    ramfs_create_static_file("date", date_elf_data, date_elf_size);
    ramfs_create_static_file("printenv", printenv_elf_data, printenv_elf_size);
    ramfs_create_static_file("cputime", cputime_elf_data, cputime_elf_size);
    ramfs_create_static_file("shmtest", shmtest_elf_data, shmtest_elf_size);
    ramfs_create_static_file("semtest", semtest_elf_data, semtest_elf_size);
    ramfs_create_static_file("mmaptest", mmaptest_elf_data, mmaptest_elf_size);
    ramfs_create_static_file("threadtest", threadtest_elf_data, threadtest_elf_size);
    ramfs_create_static_file("threadexit", threadexit_elf_data, threadexit_elf_size);
    ramfs_create_static_file("execguard", execguard_elf_data, execguard_elf_size);
    ramfs_create_static_file("ramgrow", ramgrow_elf_data, ramgrow_elf_size);
    ramfs_create_static_file("pathlim", pathlim_elf_data, pathlim_elf_size);
    ramfs_create_static_file("redirref", redirref_elf_data, redirref_elf_size);
    ramfs_create_static_file("fatref", fatref_elf_data, fatref_elf_size);
    ramfs_create_static_file("forkredir", forkredir_elf_data, forkredir_elf_size);
    ramfs_create_static_file("sigretguard", sigretguard_elf_data, sigretguard_elf_size);
    ramfs_create_static_file("killthread", killthread_elf_data, killthread_elf_size);
    ramfs_create_static_file("killwait", killwait_elf_data, killwait_elf_size);
    ramfs_create_static_file("sigflags", sigflags_elf_data, sigflags_elf_size);
    ramfs_create_static_file("fatgrow", fatgrow_elf_data, fatgrow_elf_size);
    ramfs_create_static_file("bigseek", bigseek_elf_data, bigseek_elf_size);
    ramfs_create_static_file("stress", stress_elf_data, stress_elf_size);
    ramfs_create_static_file("ush", ush_elf_data, ush_elf_size);
    ramfs_create_static_file("readme.txt", readme_text, sizeof(readme_text) - 1);
    terminal_writestring("Loaded RAMFS files: ... forkredir, sigretguard, killthread, killwait, bigseek, fatgrow, sigflags, stress, ush, readme.txt\n");

    __asm__ volatile("sti");
    kernel_shell();
}
