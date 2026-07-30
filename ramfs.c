#include "ramfs.h"
#include "heap.h"
#include "utils.h"

#define MAX_RAMFS_NODES 64
#define MAX_RAMFS_PATH  128
#define RAMFS_DYNAMIC   0x01
#define RAMFS_OPEN_REF  0x02

typedef struct {
    fs_node_t *node;
    fs_node_t *parent;
    /* Allocated size of node->ptr for a dynamic file (>= node->length), so
     * appends can reuse spare capacity instead of reallocating the whole file
     * on every growth. 0 for directories, static files, and freshly created
     * dynamic files (no buffer yet). Invariant for dynamic files: the bytes in
     * [node->length, capacity) are always zero, so a later write that grows
     * into that spare region needs no extra zero-fill. */
    uint32_t capacity;
} ramfs_entry_t;

static ramfs_entry_t ramfs_nodes[MAX_RAMFS_NODES];
static int node_count = 0;
static uint32_t next_inode = 1;
static dirent_t ramfs_dirent;

static fs_node_t *ramfs_find_child(fs_node_t *parent, const char *name) {
    if (!parent || parent->flags != FS_DIRECTORY || !name) return NULL;

    for (int i = 0; i < node_count; i++) {
        if (ramfs_nodes[i].parent == parent &&
            strcmp(ramfs_nodes[i].node->name, name) == 0) {
            return ramfs_nodes[i].node;
        }
    }
    return NULL;
}

static int ramfs_component_valid(const char *name) {
    size_t length;

    if (!name) return 0;

    length = strlen(name);
    return length > 0 && length < sizeof(ramfs_nodes[0].node->name) &&
           strcmp(name, ".") != 0 && strcmp(name, "..") != 0;
}

static int ramfs_parse_component(const char **cursor, char *component) {
    size_t length = 0;

    while (**cursor && **cursor != '/') {
        if (length + 1 >= sizeof(ramfs_nodes[0].node->name)) return 0;
        component[length++] = **cursor;
        (*cursor)++;
    }
    component[length] = '\0';
    return ramfs_component_valid(component);
}

static fs_node_t *ramfs_resolve_path(const char *path) {
    const char *cursor = path;
    fs_node_t *node = fs_root;
    char component[128];

    if (!path || !fs_root || strlen(path) >= MAX_RAMFS_PATH) return NULL;
    if (*cursor == '/') cursor++;
    if (*cursor == '\0') return node;

    while (*cursor) {
        if (!ramfs_parse_component(&cursor, component)) return NULL;
        node = ramfs_find_child(node, component);
        if (!node) return NULL;

        if (*cursor == '/') {
            cursor++;
            if (*cursor == '\0' || *cursor == '/') return NULL;
        }
    }
    return node;
}

static fs_node_t *ramfs_resolve_parent(const char *path, char *name) {
    const char *cursor = path;
    fs_node_t *parent = fs_root;
    char component[128];

    if (!path || !fs_root || strlen(path) == 0 ||
        strlen(path) >= MAX_RAMFS_PATH) {
        return NULL;
    }
    if (*cursor == '/') cursor++;
    if (*cursor == '\0') return NULL;

    while (*cursor) {
        if (!ramfs_parse_component(&cursor, component)) return NULL;

        if (*cursor == '\0') {
            strcpy(name, component);
            return parent;
        }

        cursor++;
        if (*cursor == '\0' || *cursor == '/') return NULL;

        parent = ramfs_find_child(parent, component);
        if (!parent || parent->flags != FS_DIRECTORY) return NULL;
    }
    return NULL;
}

static int ramfs_find_index(fs_node_t *node) {
    for (int i = 0; i < node_count; i++) {
        if (ramfs_nodes[i].node == node) return i;
    }
    return -1;
}

static int ramfs_has_children(fs_node_t *parent) {
    for (int i = 0; i < node_count; i++) {
        if (ramfs_nodes[i].parent == parent) return 1;
    }
    return 0;
}

static void ramfs_open(fs_node_t *node) {
    if (node && node->impl <= 0xFFFFFFFDU) node->impl += RAMFS_OPEN_REF;
}

static void ramfs_close(fs_node_t *node) {
    if (node && node->impl >= RAMFS_OPEN_REF) node->impl -= RAMFS_OPEN_REF;
}

static uint32_t ramfs_read(fs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    if (offset >= node->length) return 0;
    if (size > node->length - offset) {
        size = node->length - offset;
    }
    
    uint8_t *file_data = (uint8_t*)node->ptr;
    memcpy(buffer, file_data + offset, size);
    return size;
}

static uint32_t ramfs_write(fs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    if (size == 0) return 0;
    if (size > 0xFFFFFFFFU - offset) return 0;

    uint32_t new_length = offset + size;

    /* Grow the backing buffer only when the write extends past the current
     * length. To keep repeated appends amortized O(1) instead of O(n) per
     * write (the file was previously reallocated and fully copied on every
     * growth, i.e. O(n^2) to build up), grow the allocation geometrically and
     * remember the spare capacity in the node's ramfs entry. */
    if (new_length > node->length) {
        int idx = ramfs_find_index(node);
        uint32_t old_length = node->length;
        if (idx < 0) return 0;   /* not a tracked node */

        if (new_length > ramfs_nodes[idx].capacity) {
            uint32_t new_cap = ramfs_nodes[idx].capacity ?
                               ramfs_nodes[idx].capacity : 64;
            /* Stop doubling once another step would wrap past 2^32 and ask for
             * the exact size instead. The guard has to fire BEFORE the multiply:
             * at new_cap == 0x80000000 the doubling wraps to 0, and 0 never
             * reaches new_length nor trips a `> 0x80000000` test, so the loop
             * spins forever. Reachable from user space -- seek to 0x7FFFFFFF
             * (the largest sys_seek allows) and write a couple of bytes -- and
             * int 0x80 is an interrupt gate, so it would spin with interrupts
             * off: no timer, no keyboard, no scheduler. The whole machine. */
            while (new_cap < new_length) {
                if (new_cap > 0xFFFFFFFFU / 2) { new_cap = new_length; break; }
                new_cap *= 2;
            }

            uint8_t *new_data = (uint8_t*)kmalloc(new_cap);
            if (!new_data) {                 /* fall back to an exact fit */
                new_cap = new_length;
                new_data = (uint8_t*)kmalloc(new_cap);
                if (!new_data) return 0;     /* out of memory */
            }
            if (node->ptr) {
                memcpy(new_data, node->ptr, old_length);
                kfree(node->ptr);
            }
            /* Zero everything past the old contents so the invariant
             * "[length, capacity) is zero" holds for the new buffer. */
            memset(new_data + old_length, 0, new_cap - old_length);
            node->ptr = new_data;
            ramfs_nodes[idx].capacity = new_cap;
        } else if (offset > old_length) {
            /* Reusing spare capacity: the gap [old_length, offset) is already
             * zero by the invariant, but make it explicit for clarity/safety. */
            memset((uint8_t*)node->ptr + old_length, 0, offset - old_length);
        }
        node->length = new_length;
    }

    memcpy((uint8_t*)node->ptr + offset, buffer, size);
    return size;
}

static dirent_t *ramfs_readdir(fs_node_t *node, uint32_t index) {
    uint32_t child_index = 0;

    if (!node || node->flags != FS_DIRECTORY) return NULL;

    for (int i = 0; i < node_count; i++) {
        fs_node_t *child = ramfs_nodes[i].node;

        if (ramfs_nodes[i].parent != node) continue;
        if (child_index++ != index) continue;

        strcpy(ramfs_dirent.name, child->name);
        ramfs_dirent.inode = child->inode;
        return &ramfs_dirent;
    }
    return NULL;
}

static fs_node_t *ramfs_finddir(fs_node_t *node, const char *name) {
    if (!ramfs_component_valid(name)) return NULL;
    return ramfs_find_child(node, name);
}

static fs_node_t *ramfs_create(fs_node_t *parent, const char *name);
static int ramfs_unlink(fs_node_t *parent, const char *name);
static int ramfs_mkdir(fs_node_t *parent, const char *name);
static int ramfs_rmdir(fs_node_t *parent, const char *name);
static int ramfs_remove_node(fs_node_t *node);

static fs_node_t *ramfs_create_node(fs_node_t *parent, const char *name,
                                    uint32_t flags, uint32_t impl) {
    fs_node_t *node;

    if (!ramfs_component_valid(name) || node_count >= MAX_RAMFS_NODES ||
        (parent && ramfs_find_child(parent, name))) {
        return NULL;
    }

    node = (fs_node_t *)kmalloc(sizeof(fs_node_t));
    if (!node) return NULL;

    memset(node, 0, sizeof(fs_node_t));
    strcpy(node->name, name);
    node->flags = flags;
    node->inode = next_inode++;
    node->impl = impl;

    if (flags == FS_DIRECTORY) {
        node->readdir = ramfs_readdir;
        node->finddir = ramfs_finddir;
        node->create = ramfs_create;
        node->unlink = ramfs_unlink;
        node->mkdir = ramfs_mkdir;
        node->rmdir = ramfs_rmdir;
    } else {
        node->read = ramfs_read;
        node->write = ramfs_write;
        node->open = ramfs_open;
        node->close = ramfs_close;
    }

    ramfs_nodes[node_count].node = node;
    ramfs_nodes[node_count].parent = parent;
    ramfs_nodes[node_count].capacity = 0;
    node_count++;
    return node;
}

static fs_node_t *ramfs_create(fs_node_t *parent, const char *name) {
    if (ramfs_find_index(parent) < 0) return NULL;
    return ramfs_create_node(parent, name, FS_FILE, RAMFS_DYNAMIC);
}

static int ramfs_unlink(fs_node_t *parent, const char *name) {
    fs_node_t *node;

    if (ramfs_find_index(parent) < 0) return -1;
    node = ramfs_find_child(parent, name);
    if (!node || node->flags != FS_FILE) return -1;
    return ramfs_remove_node(node);
}

static int ramfs_mkdir(fs_node_t *parent, const char *name) {
    if (ramfs_find_index(parent) < 0) return -1;
    return ramfs_create_node(parent, name, FS_DIRECTORY, RAMFS_DYNAMIC) ? 0 : -1;
}

static int ramfs_rmdir(fs_node_t *parent, const char *name) {
    fs_node_t *node;

    if (ramfs_find_index(parent) < 0) return -1;
    node = ramfs_find_child(parent, name);
    if (!node || node == fs_root || node->flags != FS_DIRECTORY ||
        ramfs_has_children(node)) {
        return -1;
    }
    return ramfs_remove_node(node);
}

fs_node_t *ramfs_create_file(const char *path) {
    char name[128];
    fs_node_t *parent = ramfs_resolve_parent(path, name);

    if (!parent) return NULL;
    return ramfs_create_node(parent, name, FS_FILE, RAMFS_DYNAMIC);
}

/* Register a read-only file backed directly by a static buffer (no kmalloc copy).
 * The caller must ensure that `data` remains valid for the lifetime of the OS. */
fs_node_t *ramfs_create_static_file(const char *path, const uint8_t *data, uint32_t size) {
    fs_node_t *node = ramfs_create_file(path);

    if (!node) return NULL;

    node->impl   = 0;
    node->write  = NULL;          /* read-only */
    node->ptr    = (void *)data;  /* direct pointer into kernel .rodata */
    node->length = size;
    return node;
}

fs_node_t *ramfs_find_node(const char *path) {
    return ramfs_resolve_path(path);
}

fs_node_t *ramfs_find_file(const char *path) {
    fs_node_t *node = ramfs_resolve_path(path);
    return node && node->flags == FS_FILE ? node : NULL;
}

static int ramfs_remove_node(fs_node_t *node) {
    int index = ramfs_find_index(node);

    if (index <= 0) return -1;

    if (!(node->impl & RAMFS_DYNAMIC) ||
        (node->impl & ~RAMFS_DYNAMIC) != 0) {
        return -1;
    }

    kfree(node->ptr);
    kfree(node);

    for (int i = index; i + 1 < node_count; i++) {
        ramfs_nodes[i] = ramfs_nodes[i + 1];
    }
    ramfs_nodes[--node_count].node = NULL;
    ramfs_nodes[node_count].parent = NULL;
    ramfs_nodes[node_count].capacity = 0;
    return 0;
}

int ramfs_unlink_file(const char *path) {
    fs_node_t *node = ramfs_resolve_path(path);

    if (!node || node->flags != FS_FILE) return -1;
    return ramfs_remove_node(node);
}

int ramfs_create_directory(const char *path) {
    char name[128];
    fs_node_t *parent = ramfs_resolve_parent(path, name);

    if (!parent) return -1;
    return ramfs_create_node(parent, name, FS_DIRECTORY, RAMFS_DYNAMIC) ? 0 : -1;
}

int ramfs_remove_directory(const char *path) {
    fs_node_t *node = ramfs_resolve_path(path);

    if (!node || node == fs_root || node->flags != FS_DIRECTORY ||
        ramfs_has_children(node)) {
        return -1;
    }
    return ramfs_remove_node(node);
}

int ramfs_mount(const char *path, fs_node_t *node) {
    char name[128];
    fs_node_t *parent = ramfs_resolve_parent(path, name);

    if (!parent || !node || node_count >= MAX_RAMFS_NODES ||
        ramfs_find_child(parent, name)) {
        return -1;
    }

    strcpy(node->name, name);
    ramfs_nodes[node_count].node = node;
    ramfs_nodes[node_count].parent = parent;
    ramfs_nodes[node_count].capacity = 0;
    node_count++;
    return 0;
}

uint32_t ramfs_get_node_count(void) {
    return (uint32_t)node_count;
}

void ramfs_init(void) {
    if (!fs_root) fs_root = ramfs_create_node(NULL, "root", FS_DIRECTORY, 0);
}
