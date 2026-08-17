#include "fat16.h"
#include "pmm.h"
#include "utils.h"

/*
 * Minimal FAT16 driver. Parses a standard FAT16 image (BPB, FAT, 8.3 directory
 * entries, cluster chains). The embedded image is copied into a writable RAM
 * buffer at mount time, so files can be created, written and removed; the
 * on-disk format is the real FAT16 layout (the same code would drive an
 * ATA-backed disk by pointing the byte source at sector reads/writes).
 */

#define FAT16_DIR_ATTR_LFN    0x0F
#define FAT16_DIR_ATTR_VOLUME 0x08
#define FAT16_DIR_ATTR_DIR    0x10
#define FAT16_DIR_ATTR_FILE   0x20
#define FAT16_RESERVED_START  0xFFF0u
#define FAT16_EOC             0xFFF8u   /* >= this means end of chain */
#define FAT16_EOC_MARK        0xFFFFu
#define FAT16_MAX_DIR_SLOTS   8192      /* guard against corrupt cyclic chains */
#define FAT16_NODE_POOL       16

typedef struct {
    uint8_t *img;
    uint32_t img_size;
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entries;
    uint16_t sectors_per_fat;
    uint32_t fat_lba;
    uint32_t root_dir_lba;
    uint32_t data_lba;
    uint32_t bytes_per_cluster;
    uint32_t max_cluster;       /* highest usable cluster number + 1 */
    int      mounted;
} fat16_fs_t;

static fat16_fs_t fs;
static fs_node_t fat16_root_node;
static fs_node_t fat16_node_pool[FAT16_NODE_POOL];
/* Open-descriptor count per pool slot. RAMFS and DiskFS both track this (via
 * the node's impl bits and diskfs_open_refs respectively); FAT16 needs it for
 * the same two reasons: a slot that is still open must not be recycled to
 * describe a different file, and a file that is still open must not have its
 * cluster chain freed by unlink (those clusters could be handed to a new file
 * while the old descriptor still reads through them). */
static uint32_t fat16_node_refs[FAT16_NODE_POOL];
static uint32_t fat16_node_next;
static dirent_t fat16_dirent;

/* --- little-endian access with bounds checks ----------------------------- */
static uint16_t rd16(uint32_t off) {
    if (off + 2 > fs.img_size) return 0;
    return (uint16_t)(fs.img[off] | (fs.img[off + 1] << 8));
}

static uint32_t rd32(uint32_t off) {
    if (off + 4 > fs.img_size) return 0;
    return (uint32_t)fs.img[off] | ((uint32_t)fs.img[off + 1] << 8) |
           ((uint32_t)fs.img[off + 2] << 16) | ((uint32_t)fs.img[off + 3] << 24);
}

static void wr16(uint32_t off, uint16_t value) {
    if (off + 2 > fs.img_size) return;
    fs.img[off] = (uint8_t)value;
    fs.img[off + 1] = (uint8_t)(value >> 8);
}

static void wr32(uint32_t off, uint32_t value) {
    if (off + 4 > fs.img_size) return;
    fs.img[off] = (uint8_t)value;
    fs.img[off + 1] = (uint8_t)(value >> 8);
    fs.img[off + 2] = (uint8_t)(value >> 16);
    fs.img[off + 3] = (uint8_t)(value >> 24);
}

static uint32_t fat16_fat_off(uint32_t cluster) {
    return fs.fat_lba * fs.bytes_per_sector + cluster * 2;
}

static uint32_t fat16_next_cluster(uint32_t cluster) {
    return rd16(fat16_fat_off(cluster));
}

static void fat16_set_cluster(uint32_t cluster, uint16_t value) {
    wr16(fat16_fat_off(cluster), value);
}

static int fat16_cluster_valid(uint32_t cluster) {
    return cluster >= 2 && cluster < fs.max_cluster &&
           cluster < FAT16_RESERVED_START;
}

/* Validate a complete chain before a destructive traversal. Limiting the
 * walk to the number of usable clusters rejects cycles without extra memory. */
static int fat16_chain_valid(uint32_t first) {
    uint32_t cluster = first;
    uint32_t end = fs.max_cluster < FAT16_RESERVED_START ?
                   fs.max_cluster : FAT16_RESERVED_START;
    uint32_t limit = end > 2 ? end - 2 : 0;

    if (cluster == 0) return 1;           /* empty file */
    for (uint32_t steps = 0; steps < limit; steps++) {
        uint32_t next;

        if (!fat16_cluster_valid(cluster)) return 0;
        next = fat16_next_cluster(cluster);
        if (next >= FAT16_EOC) return 1;
        cluster = next;
    }
    return 0;                              /* longer than the volume: a cycle */
}

static uint32_t fat16_cluster_lba(uint32_t cluster) {
    return fs.data_lba + (cluster - 2) * fs.sectors_per_cluster;
}

static uint32_t fat16_cluster_off(uint32_t cluster) {
    return fat16_cluster_lba(cluster) * fs.bytes_per_sector;
}

/* Allocate a free cluster (FAT entry 0), mark it end-of-chain, zero its data. */
static uint32_t fat16_alloc_cluster(void) {
    for (uint32_t c = 2; c < fs.max_cluster; c++) {
        if (rd16(fat16_fat_off(c)) == 0) {
            uint32_t off = fat16_cluster_off(c);
            fat16_set_cluster(c, FAT16_EOC_MARK);
            if (off + fs.bytes_per_cluster <= fs.img_size)
                memset(fs.img + off, 0, fs.bytes_per_cluster);
            return c;
        }
    }
    return 0;
}

/* Image pointer of the logical_index-th 32-byte slot in a directory.
 * dir_cluster == 0 means the fixed root directory. NULL past the end. */
static uint8_t *fat16_entry_ptr(uint32_t dir_cluster, uint32_t logical_index) {
    uint32_t off;

    if (dir_cluster == 0) {
        if (logical_index >= fs.root_entries) return NULL;
        off = fs.root_dir_lba * fs.bytes_per_sector + logical_index * 32;
    } else {
        uint32_t epc = fs.bytes_per_cluster / 32;
        uint32_t cluster_index = logical_index / epc;
        uint32_t entry_in_cluster = logical_index % epc;
        uint32_t cluster = dir_cluster;

        for (uint32_t i = 0; i < cluster_index; i++) {
            cluster = fat16_next_cluster(cluster);
            if (cluster < 2 || cluster >= FAT16_EOC) return NULL;
        }
        off = fat16_cluster_off(cluster) + entry_in_cluster * 32;
    }

    if (off + 32 > fs.img_size) return NULL;
    return fs.img + off;
}

static char fat16_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static char fat16_upper(char c) {
    return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
}

/* Convert an 8.3 directory entry name into "name.ext" (lowercase). */
static void fat16_format_name(const uint8_t *entry, char *out) {
    int p = 0, has_ext = 0;

    for (int i = 0; i < 8 && entry[i] != ' '; i++) out[p++] = fat16_lower(entry[i]);
    for (int i = 8; i < 11; i++) {
        if (entry[i] != ' ') { has_ext = 1; break; }
    }
    if (has_ext) {
        out[p++] = '.';
        for (int i = 8; i < 11 && entry[i] != ' '; i++)
            out[p++] = fat16_lower(entry[i]);
    }
    out[p] = '\0';
}

/* Encode an arbitrary name into 8.3 fields (uppercase, space-padded).
 * Returns 0 on success, -1 if it does not fit 8.3. */
static int fat16_make_83(const char *name, uint8_t *field) {
    int len = 0, dot = -1, name_len, ext_start, ext_len;

    while (name[len]) len++;
    for (int i = len - 1; i >= 0; i--) {
        if (name[i] == '.') { dot = i; break; }
    }
    name_len = (dot >= 0) ? dot : len;
    ext_start = (dot >= 0) ? dot + 1 : len;
    ext_len = len - ext_start;
    if (name_len == 0 || name_len > 8 || ext_len > 3) return -1;

    for (int i = 0; i < 11; i++) field[i] = ' ';
    for (int i = 0; i < name_len; i++) field[i] = (uint8_t)fat16_upper(name[i]);
    for (int i = 0; i < ext_len; i++)
        field[8 + i] = (uint8_t)fat16_upper(name[ext_start + i]);
    return 0;
}

/* Returns 1 if the entry is a real file/dir, 0 to skip, -1 to stop (end). */
static int fat16_entry_kind(const uint8_t *e) {
    if (e[0] == 0x00) return -1;
    if (e[0] == 0xE5) return 0;
    if ((e[11] & FAT16_DIR_ATTR_LFN) == FAT16_DIR_ATTR_LFN) return 0;
    if (e[11] & FAT16_DIR_ATTR_VOLUME) return 0;
    if (e[0] == '.') return 0;
    return 1;
}

static uint32_t fat16_vfs_read(fs_node_t *node, uint32_t offset,
                               uint32_t size, uint8_t *buffer);
static uint32_t fat16_vfs_write(fs_node_t *node, uint32_t offset,
                                uint32_t size, uint8_t *buffer);
static void fat16_vfs_open(fs_node_t *node);
static void fat16_vfs_close(fs_node_t *node);
static dirent_t *fat16_vfs_readdir(fs_node_t *node, uint32_t index);
static fs_node_t *fat16_vfs_finddir(fs_node_t *node, const char *name);
static fs_node_t *fat16_vfs_create(fs_node_t *node, const char *name);
static int fat16_vfs_unlink(fs_node_t *node, const char *name);

/* Build (or recycle) a node describing a directory entry. node->ptr points at
 * the entry inside the writable image so writes can update its size/cluster.
 *
 * fs_node_t pointers handed out here are held for as long as a caller keeps
 * them (e.g. syscall.c's open_file_t.node for an open fd), but the pool below
 * has no reference counting -- it's a fixed-size, system-wide cache. If this
 * always advanced the round-robin cursor, more than FAT16_NODE_POOL lookups
 * in a session (across any files/directories, not just this one) would
 * silently repoint a still-open fd's node at an unrelated file. Reusing the
 * existing slot for the same directory entry (matched by `e`, its byte offset
 * in the image) keeps repeated lookups of an already-tracked file identity
 * stable; only a lookup of a *new* entry consumes a fresh slot. */
static fs_node_t *fat16_make_node(uint8_t *e, const char *name) {
    fs_node_t *node;
    uint32_t slot = FAT16_NODE_POOL;

    /* Reuse the slot already describing this directory entry, so repeated
     * lookups of one file keep the same node identity (and its open count). */
    for (uint32_t i = 0; i < FAT16_NODE_POOL; i++) {
        if (fat16_node_pool[i].ptr == e) { slot = i; break; }
    }

    /* Otherwise claim a slot that nobody holds open. Recycling a slot with
     * live descriptors would silently repoint them at a different file, so
     * fail the lookup instead -- callers treat NULL as "not found". */
    if (slot == FAT16_NODE_POOL) {
        for (uint32_t n = 0; n < FAT16_NODE_POOL; n++) {
            uint32_t i = (fat16_node_next + n) % FAT16_NODE_POOL;
            if (fat16_node_refs[i] == 0) { slot = i; break; }
        }
        if (slot == FAT16_NODE_POOL) return NULL;   /* every node is open */
        fat16_node_next = (slot + 1) % FAT16_NODE_POOL;
    }

    node = &fat16_node_pool[slot];
    memset(node, 0, sizeof(*node));
    strcpy(node->name, name);
    node->impl = rd16((uint32_t)(e - fs.img) + 26);   /* first cluster */
    node->inode = node->impl + 1;
    node->ptr = e;                                     /* directory entry */

    if (e[11] & FAT16_DIR_ATTR_DIR) {
        node->flags = FS_DIRECTORY;
        node->readdir = fat16_vfs_readdir;
        node->finddir = fat16_vfs_finddir;
        node->create = fat16_vfs_create;
        node->unlink = fat16_vfs_unlink;
    } else {
        node->flags = FS_FILE;
        node->length = rd32((uint32_t)(e - fs.img) + 28);
        node->read = fat16_vfs_read;
        node->write = fat16_vfs_write;
        node->open = fat16_vfs_open;
        node->close = fat16_vfs_close;
    }
    return node;
}

/* Index of `node` in the pool, or -1 (the /fat root node is not pooled). */
static int fat16_node_slot(const fs_node_t *node) {
    for (uint32_t i = 0; i < FAT16_NODE_POOL; i++) {
        if (&fat16_node_pool[i] == node) return (int)i;
    }
    return -1;
}

static void fat16_vfs_open(fs_node_t *node) {
    int slot = fat16_node_slot(node);
    if (slot >= 0) fat16_node_refs[slot]++;
}

static void fat16_vfs_close(fs_node_t *node) {
    int slot = fat16_node_slot(node);
    if (slot >= 0 && fat16_node_refs[slot] > 0) fat16_node_refs[slot]--;
}

/* True if any open descriptor still refers to the directory entry at `e`. */
static int fat16_entry_is_open(const uint8_t *e) {
    for (uint32_t i = 0; i < FAT16_NODE_POOL; i++) {
        if (fat16_node_pool[i].ptr == e && fat16_node_refs[i] > 0) return 1;
    }
    return 0;
}

static dirent_t *fat16_vfs_readdir(fs_node_t *node, uint32_t index) {
    uint32_t found = 0;

    if (!fs.mounted || !node) return NULL;
    for (uint32_t logical = 0; logical < FAT16_MAX_DIR_SLOTS; logical++) {
        uint8_t *e = fat16_entry_ptr(node->impl, logical);
        int kind;

        if (!e) return NULL;
        kind = fat16_entry_kind(e);
        if (kind < 0) return NULL;
        if (kind == 0) continue;
        if (found++ == index) {
            fat16_format_name(e, fat16_dirent.name);
            fat16_dirent.inode = rd16((uint32_t)(e - fs.img) + 26) + 1;
            return &fat16_dirent;
        }
    }
    return NULL;
}

static int fat16_name_eq(const char *a, const char *b) {
    while (*a && *b) {
        if (fat16_lower(*a) != fat16_lower(*b)) return 0;
        a++; b++;
    }
    return *a == *b;
}

static fs_node_t *fat16_vfs_finddir(fs_node_t *node, const char *name) {
    if (!fs.mounted || !node || !name) return NULL;
    for (uint32_t logical = 0; logical < FAT16_MAX_DIR_SLOTS; logical++) {
        uint8_t *e = fat16_entry_ptr(node->impl, logical);
        char entry_name[16];
        int kind;

        if (!e) return NULL;
        kind = fat16_entry_kind(e);
        if (kind < 0) return NULL;
        if (kind == 0) continue;
        fat16_format_name(e, entry_name);
        if (fat16_name_eq(entry_name, name)) return fat16_make_node(e, entry_name);
    }
    return NULL;
}

/* Find a free directory slot (empty or deleted) in the directory. */
static uint8_t *fat16_free_slot(uint32_t dir_cluster) {
    for (uint32_t logical = 0; logical < FAT16_MAX_DIR_SLOTS; logical++) {
        uint8_t *e = fat16_entry_ptr(dir_cluster, logical);
        if (!e) return NULL;
        if (e[0] == 0x00 || e[0] == 0xE5) return e;
    }
    return NULL;
}

static fs_node_t *fat16_vfs_create(fs_node_t *node, const char *name) {
    uint8_t field[11];
    uint8_t *slot;

    if (!fs.mounted || !node || fat16_make_83(name, field) != 0) return NULL;
    if (fat16_vfs_finddir(node, name)) return NULL;   /* already exists */

    slot = fat16_free_slot(node->impl);
    if (!slot) return NULL;

    memset(slot, 0, 32);
    memcpy(slot, field, 11);
    slot[11] = FAT16_DIR_ATTR_FILE;
    wr16((uint32_t)(slot - fs.img) + 26, 0);   /* no cluster yet */
    wr32((uint32_t)(slot - fs.img) + 28, 0);   /* size 0 */
    return fat16_make_node(slot, name);
}

static uint32_t fat16_vfs_write(fs_node_t *node, uint32_t offset,
                                uint32_t size, uint8_t *buffer) {
    uint8_t *entry;
    uint32_t bpc = fs.bytes_per_cluster;
    uint32_t first, end, needed, cluster, count, written = 0;
    uint32_t first_new = 0, new_parent = 0, new_count = 0;

    if (!fs.mounted || !node || !node->ptr || (!buffer && size != 0)) return 0;
    if (size == 0) return 0;

    /* No cluster in this volume can cover the requested first byte. Reject it
     * before an empty file acquires a chain that can never reach the offset. */
    if (fs.max_cluster <= 2 || offset / bpc >= fs.max_cluster - 2) return 0;
    if (size > UINT32_MAX - offset) return 0;

    entry = (uint8_t *)node->ptr;
    first = rd16((uint32_t)(entry - fs.img) + 26);
    end = offset + size;
    needed = end / bpc + (end % bpc != 0); /* clusters from the start */

    if (first == 0) {
        first = fat16_alloc_cluster();
        if (!first) return 0;
        first_new = first;
        new_count = 1;
        wr16((uint32_t)(entry - fs.img) + 26, (uint16_t)first);
        node->impl = first;
    }

    /* Extend the chain so it spans `needed` clusters. */
    cluster = first;
    count = 1;
    while (count < needed) {
        uint32_t next = fat16_next_cluster(cluster);
        if (next < 2 || next >= FAT16_EOC) {
            next = fat16_alloc_cluster();
            if (!next) break;
            if (!first_new) {
                first_new = next;
                new_parent = cluster;
            }
            new_count++;
            fat16_set_cluster(cluster, (uint16_t)next);
        }
        cluster = next;
        count++;
    }

    /* Walk the chain copying the bytes that fall in [offset, end). */
    cluster = first;
    for (uint32_t cl = 0; cl < needed && written < size; cl++) {
        uint32_t cluster_start = cl * bpc;
        uint32_t cluster_end = cluster_start + bpc;

        if (cluster < 2 || cluster >= FAT16_EOC) break;
        if (cluster_end > offset && cluster_start < end) {
            uint32_t lo = offset > cluster_start ? offset : cluster_start;
            uint32_t hi = end < cluster_end ? end : cluster_end;
            uint32_t dst = fat16_cluster_off(cluster) + (lo - cluster_start);
            uint32_t n = hi - lo;

            if (dst + n <= fs.img_size) {
                memcpy(fs.img + dst, buffer + (lo - offset), n);
                written += n;
            }
        }
        if (cl + 1 < needed) cluster = fat16_next_cluster(cluster);
    }

    /* Record only the bytes actually stored. Both loops above can stop early
     * when the volume runs out of free clusters (fat16_alloc_cluster returns
     * 0), in which case `written` is short of `size`. Claiming `end` here
     * would leave the directory entry advertising data that was never written,
     * so subsequent reads would report a length the cluster chain cannot back.
     * The written bytes are contiguous from `offset`, so offset + written is
     * the true end of file data. When the write fully succeeds this is exactly
     * `end`, i.e. the normal path is unchanged.
     *
     * That identity only holds when something WAS written. With written == 0 --
     * a write starting past the last cluster the chain could be extended to --
     * offset + written is merely the offset the caller asked for, and nothing
     * marks the end of anything. Recording it grew the file to the seek offset
     * without storing a byte, so the file then claimed a length its own chain
     * could not back and reads returned the tail of the last real cluster as
     * if it were file data. A write that stores nothing must change nothing. */
    if (written == 0) {
        /* Extending toward an unreachable offset may have consumed every free
         * cluster before allocation failed. A zero-byte write is a no-op, so
         * detach and free exactly the clusters allocated by this call. */
        if (first_new) {
            if (new_parent) {
                fat16_set_cluster(new_parent, FAT16_EOC_MARK);
            } else {
                wr16((uint32_t)(entry - fs.img) + 26, 0);
                node->impl = 0;
            }
            cluster = first_new;
            for (uint32_t i = 0;
                 i < new_count && cluster >= 2 && cluster < fs.max_cluster;
                 i++) {
                uint32_t next = fat16_next_cluster(cluster);
                fat16_set_cluster(cluster, 0);
                cluster = next;
            }
        }
        return 0;
    }

    {
        uint32_t written_end = offset + written;
        if (written_end > rd32((uint32_t)(entry - fs.img) + 28)) {
            wr32((uint32_t)(entry - fs.img) + 28, written_end);
            node->length = written_end;
        }
    }
    return written;
}

static int fat16_vfs_unlink(fs_node_t *node, const char *name) {
    uint8_t *e = NULL;

    if (!fs.mounted || !node || !name) return -1;
    for (uint32_t logical = 0; logical < FAT16_MAX_DIR_SLOTS; logical++) {
        uint8_t *cur = fat16_entry_ptr(node->impl, logical);
        char entry_name[16];
        int kind;

        if (!cur) break;
        kind = fat16_entry_kind(cur);
        if (kind < 0) break;
        if (kind == 0) continue;
        fat16_format_name(cur, entry_name);
        if (fat16_name_eq(entry_name, name)) { e = cur; break; }
    }
    if (!e || (e[11] & FAT16_DIR_ATTR_DIR)) return -1;   /* files only */

    /* Refuse while the file is still open, matching RAMFS and DiskFS. Freeing
     * the chain now would return those clusters to the free pool, so a later
     * write could hand them to a different file while the existing descriptor
     * still reads through them -- returning another file's contents. */
    if (fat16_entry_is_open(e)) return -1;

    /* Free the cluster chain, then mark the entry deleted. */
    uint32_t cluster = rd16((uint32_t)(e - fs.img) + 26);
    if (!fat16_chain_valid(cluster)) return -1;
    while (fat16_cluster_valid(cluster)) {
        uint32_t next = fat16_next_cluster(cluster);
        fat16_set_cluster(cluster, 0);
        if (next >= FAT16_EOC) break;
        cluster = next;
    }
    e[0] = 0xE5;
    return 0;
}

static uint32_t fat16_vfs_read(fs_node_t *node, uint32_t offset,
                               uint32_t size, uint8_t *buffer) {
    uint32_t cluster, pos, copied = 0;
    uint32_t bpc = fs.bytes_per_cluster;

    if (!fs.mounted || !node || !buffer) return 0;
    if (offset >= node->length) return 0;
    if (size > node->length - offset) size = node->length - offset;

    cluster = node->impl;
    for (uint32_t i = 0; i < offset / bpc; i++) {
        cluster = fat16_next_cluster(cluster);
        if (cluster < 2 || cluster >= FAT16_EOC) return 0;
    }
    pos = offset % bpc;

    while (copied < size && cluster >= 2 && cluster < FAT16_EOC) {
        uint32_t cluster_off = fat16_cluster_off(cluster) + pos;
        uint32_t avail = bpc - pos;
        uint32_t chunk = size - copied;

        if (chunk > avail) chunk = avail;
        if (cluster_off + chunk > fs.img_size)
            chunk = cluster_off < fs.img_size ? fs.img_size - cluster_off : 0;
        memcpy(buffer + copied, fs.img + cluster_off, chunk);
        copied += chunk;
        pos = 0;
        if (copied < size) cluster = fat16_next_cluster(cluster);
    }
    return copied;
}

static void fat16_release_image(void) {
    uint8_t *image = fs.img;
    uint32_t blocks = fs.img_size / PMM_BLOCK_SIZE +
        (fs.img_size % PMM_BLOCK_SIZE != 0);

    memset(&fs, 0, sizeof(fs));
    if (image) pmm_free_blocks(image, blocks);
}

void fat16_install(const uint8_t *image, uint32_t size) {
    uint32_t root_dir_sectors, blocks, data_offset;
    uint8_t *copy;

    fat16_release_image();
    memset(fat16_node_pool, 0, sizeof(fat16_node_pool));
    memset(fat16_node_refs, 0, sizeof(fat16_node_refs));
    fat16_node_next = 0;
    if (!image || size < 512) return;

    /* Work on a writable RAM copy so the filesystem can be modified. */
    blocks = size / PMM_BLOCK_SIZE + (size % PMM_BLOCK_SIZE != 0);
    copy = (uint8_t *)pmm_alloc_blocks(blocks);
    if (!copy) return;
    memcpy(copy, image, size);

    fs.img = copy;
    fs.img_size = size;
    fs.bytes_per_sector = rd16(11);
    fs.sectors_per_cluster = copy[13];
    fs.reserved_sectors = rd16(14);
    fs.num_fats = copy[16];
    fs.root_entries = rd16(17);
    fs.sectors_per_fat = rd16(22);

    if (fs.bytes_per_sector != 512 || fs.sectors_per_cluster == 0 ||
        fs.num_fats == 0 || fs.root_entries == 0 || fs.sectors_per_fat == 0 ||
        copy[510] != 0x55 || copy[511] != 0xAA) {
        goto reject;
    }

    fs.fat_lba = fs.reserved_sectors;
    fs.root_dir_lba = fs.reserved_sectors + fs.num_fats * fs.sectors_per_fat;
    root_dir_sectors =
        (fs.root_entries * 32 + fs.bytes_per_sector - 1) / fs.bytes_per_sector;
    fs.data_lba = fs.root_dir_lba + root_dir_sectors;
    fs.bytes_per_cluster = fs.sectors_per_cluster * fs.bytes_per_sector;

    /* Compare in sectors before multiplying. A malformed BPB can place the
     * data region at or above 8,388,608 sectors, where a 512-byte offset
     * wraps uint32_t and otherwise appears to lie inside a small image. */
    if (fs.data_lba > fs.img_size / fs.bytes_per_sector) goto reject;
    data_offset = fs.data_lba * fs.bytes_per_sector;

    /* Usable clusters are bounded by both the FAT size and the image size. */
    uint32_t fat_clusters = fs.sectors_per_fat * fs.bytes_per_sector / 2;
    uint32_t data_clusters =
        2 + (fs.img_size - data_offset) / fs.bytes_per_cluster;
    fs.max_cluster = fat_clusters < data_clusters ? fat_clusters : data_clusters;

    memset(&fat16_root_node, 0, sizeof(fat16_root_node));
    strcpy(fat16_root_node.name, "fat");
    fat16_root_node.flags = FS_DIRECTORY;
    fat16_root_node.impl = 0;        /* 0 => fixed root directory */
    fat16_root_node.readdir = fat16_vfs_readdir;
    fat16_root_node.finddir = fat16_vfs_finddir;
    fat16_root_node.create = fat16_vfs_create;
    fat16_root_node.unlink = fat16_vfs_unlink;

    fs.mounted = 1;
    return;

reject:
    fat16_release_image();
}

int fat16_is_mounted(void) {
    return fs.mounted;
}

fs_node_t *fat16_get_root_node(void) {
    return fs.mounted ? &fat16_root_node : NULL;
}
