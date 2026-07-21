#include "procfs.h"
#include "process.h"
#include "timer.h"
#include "utils.h"

/* Generator id stored in each synthetic file node's impl field. */
#define PROC_COUNT     1
#define PROC_UPTIME    2
#define PROC_PROCESSES 3
#define PROC_SELF_NAME   10   /* /proc/self/name   */
#define PROC_SELF_STATUS 11   /* /proc/self/status */

#define PROC_FILE_COUNT 3   /* count, uptime, processes */
#define PROC_SELF_COUNT 2   /* name, status */

static fs_node_t proc_root;
static fs_node_t proc_files[PROC_FILE_COUNT];
static fs_node_t proc_self_dir;
static fs_node_t proc_self_files[PROC_SELF_COUNT];
static dirent_t  proc_dirent;
static char      gen_buf[512];

static const char *proc_names[PROC_FILE_COUNT] = { "count", "uptime", "processes" };
static const char *proc_self_names[PROC_SELF_COUNT] = { "name", "status" };
/* Root directory listing: the three files plus the "self" subdirectory. */
static const char *proc_root_entries[PROC_FILE_COUNT + 1] = {
    "count", "uptime", "processes", "self"
};

/* Write the decimal form of `v` at `out`; returns the digit count. */
static uint32_t u_to_str(uint32_t v, char *out) {
    char tmp[12];
    int n = 0;

    if (v == 0) { out[0] = '0'; return 1; }
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return (uint32_t)n;
}

static uint32_t append_str(char *out, uint32_t pos, const char *s) {
    while (*s) out[pos++] = *s++;
    return pos;
}

static char state_char(int state) {
    return state == PROCESS_ZOMBIE ? 'Z' : 'R';
}

/* Render a proc file's full contents into gen_buf; returns the length. */
static uint32_t proc_generate(uint32_t which) {
    process_info_t snap[MAX_PROCESSES];
    process_t *self;
    uint32_t pos = 0;
    uint32_t n;

    switch (which) {
    case PROC_COUNT:
        n = process_snapshot(snap, MAX_PROCESSES);
        pos += u_to_str(n, gen_buf + pos);
        gen_buf[pos++] = '\n';
        break;
    case PROC_UPTIME:
        pos += u_to_str(timer_get_ticks(), gen_buf + pos);
        gen_buf[pos++] = '\n';
        break;
    case PROC_PROCESSES:
        n = process_snapshot(snap, MAX_PROCESSES);
        for (uint32_t i = 0; i < n; i++) {
            /* Worst case: two 10-digit int32 fields, 1 state char, a full
             * PROCESS_NAME_MAX-1 name, 3 separating spaces and a newline.
             * gen_buf is a fixed 512-byte buffer; pid/ppid grow without bound
             * over the system's lifetime (next_pid never resets), so bail out
             * (truncating the listing) rather than overrun it once a line
             * might not fit, instead of trusting the sizes to stay small. */
            if (pos + 10 + 1 + 10 + 1 + 1 + 1 + (PROCESS_NAME_MAX - 1) + 1 >
                sizeof(gen_buf)) {
                break;
            }
            pos += u_to_str((uint32_t)snap[i].pid, gen_buf + pos);
            gen_buf[pos++] = ' ';
            pos += u_to_str((uint32_t)snap[i].parent_pid, gen_buf + pos);
            gen_buf[pos++] = ' ';
            gen_buf[pos++] = state_char(snap[i].state);
            gen_buf[pos++] = ' ';
            pos = append_str(gen_buf, pos, snap[i].name);
            gen_buf[pos++] = '\n';
        }
        break;
    case PROC_SELF_NAME:
        self = process_get_current();
        pos = append_str(gen_buf, pos, self ? self->name : "?");
        gen_buf[pos++] = '\n';
        break;
    case PROC_SELF_STATUS:
        self = process_get_current();
        pos = append_str(gen_buf, pos, "name=");
        pos = append_str(gen_buf, pos, self ? self->name : "?");
        pos = append_str(gen_buf, pos, " pid=");
        pos += u_to_str(self ? (uint32_t)self->pid : 0, gen_buf + pos);
        pos = append_str(gen_buf, pos, " ppid=");
        pos += u_to_str(self ? (uint32_t)self->parent_pid : 0, gen_buf + pos);
        pos = append_str(gen_buf, pos, " state=");
        gen_buf[pos++] = self ? state_char(self->state) : '?';
        pos = append_str(gen_buf, pos, " cpu=");
        pos += u_to_str(self ? self->cpu_ticks : 0, gen_buf + pos);
        gen_buf[pos++] = '\n';
        break;
    default:
        break;
    }
    return pos;
}

static uint32_t proc_read(fs_node_t *node, uint32_t offset, uint32_t size,
                          uint8_t *buffer) {
    uint32_t len = proc_generate(node->impl);

    if (offset >= len) return 0;
    if (size > len - offset) size = len - offset;
    memcpy(buffer, gen_buf + offset, size);
    return size;
}

static dirent_t *proc_readdir(fs_node_t *node, uint32_t index) {
    (void)node;
    if (index >= PROC_FILE_COUNT + 1) return NULL;
    strcpy(proc_dirent.name, proc_root_entries[index]);
    proc_dirent.inode = index + 1;
    return &proc_dirent;
}

static fs_node_t *proc_finddir(fs_node_t *node, const char *name) {
    (void)node;
    if (strcmp(name, "self") == 0) return &proc_self_dir;
    for (int i = 0; i < PROC_FILE_COUNT; i++) {
        if (strcmp(name, proc_names[i]) == 0) {
            proc_files[i].length = proc_generate(proc_files[i].impl);
            return &proc_files[i];
        }
    }
    return NULL;
}

static dirent_t *proc_self_readdir(fs_node_t *node, uint32_t index) {
    (void)node;
    if (index >= PROC_SELF_COUNT) return NULL;
    strcpy(proc_dirent.name, proc_self_names[index]);
    proc_dirent.inode = 100 + index;
    return &proc_dirent;
}

static fs_node_t *proc_self_finddir(fs_node_t *node, const char *name) {
    (void)node;
    for (int i = 0; i < PROC_SELF_COUNT; i++) {
        if (strcmp(name, proc_self_names[i]) == 0) {
            proc_self_files[i].length = proc_generate(proc_self_files[i].impl);
            return &proc_self_files[i];
        }
    }
    return NULL;
}

static void init_file(fs_node_t *node, const char *name, uint32_t impl) {
    memset(node, 0, sizeof(*node));
    strcpy(node->name, name);
    node->flags = FS_FILE;
    node->inode = impl;
    node->impl = impl;
    node->read = proc_read;
}

fs_node_t *procfs_get_root(void) {
    memset(&proc_root, 0, sizeof(proc_root));
    proc_root.flags = FS_DIRECTORY;
    proc_root.readdir = proc_readdir;
    proc_root.finddir = proc_finddir;

    for (int i = 0; i < PROC_FILE_COUNT; i++)
        init_file(&proc_files[i], proc_names[i], (uint32_t)(i + 1));

    memset(&proc_self_dir, 0, sizeof(proc_self_dir));
    strcpy(proc_self_dir.name, "self");
    proc_self_dir.flags = FS_DIRECTORY;
    proc_self_dir.readdir = proc_self_readdir;
    proc_self_dir.finddir = proc_self_finddir;

    init_file(&proc_self_files[0], "name", PROC_SELF_NAME);
    init_file(&proc_self_files[1], "status", PROC_SELF_STATUS);

    return &proc_root;
}
