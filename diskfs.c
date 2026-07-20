#include "diskfs.h"
#include "ata.h"
#include "fs.h"
#include "utils.h"

#define DISKFS_MAGIC            0x5346534DU
#define DISKFS_VERSION          2
#define DISKFS_SUPERBLOCK_LBA   4
#define DISKFS_DIRECTORY_LBA    5
#define DISKFS_DIRECTORY_SECTORS 2
#define DISKFS_DATA_LBA         16
#define DISKFS_MAX_FILES        16
#define DISKFS_FILE_NAME_SIZE   32
#define DISKFS_FILE_SECTORS     4
#define DISKFS_CHECKSUM_SEED    0xA5C35A3CU
#define DISKFS_ENTRY_FILE       1
#define DISKFS_ENTRY_DIRECTORY  2
#define DISKFS_ROOT_PARENT      0xFF

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t version;
    uint32_t generation;
    uint32_t total_sectors;
    uint32_t directory_lba;
    uint32_t directory_sectors;
    uint32_t data_lba;
    uint32_t max_files;
    uint32_t sectors_per_file;
    uint32_t checksum;
    uint8_t reserved[ATA_SECTOR_SIZE - 10 * sizeof(uint32_t)];
} diskfs_superblock_t;

typedef struct __attribute__((packed)) {
    uint8_t used;
    uint8_t type;
    uint8_t parent;
    uint8_t reserved;
    char name[DISKFS_FILE_NAME_SIZE];
    uint32_t length;
} diskfs_entry_t;

typedef char diskfs_superblock_size_check[
    sizeof(diskfs_superblock_t) == ATA_SECTOR_SIZE ? 1 : -1];
typedef char diskfs_directory_size_check[
    sizeof(diskfs_entry_t) * DISKFS_MAX_FILES <=
    DISKFS_DIRECTORY_SECTORS * ATA_SECTOR_SIZE ? 1 : -1];

static diskfs_superblock_t diskfs_superblock;
static diskfs_entry_t diskfs_entries[DISKFS_MAX_FILES];
static fs_node_t diskfs_root_node;
static fs_node_t diskfs_nodes[DISKFS_MAX_FILES];
static uint32_t diskfs_open_refs[DISKFS_MAX_FILES];
static dirent_t diskfs_dirent;
static int diskfs_mounted = 0;

static uint32_t diskfs_vfs_read(fs_node_t *node, uint32_t offset,
                                uint32_t size, uint8_t *buffer);
static uint32_t diskfs_vfs_write(fs_node_t *node, uint32_t offset,
                                 uint32_t size, uint8_t *buffer);
static void diskfs_vfs_open(fs_node_t *node);
static void diskfs_vfs_close(fs_node_t *node);
static dirent_t *diskfs_vfs_readdir(fs_node_t *node, uint32_t index);
static fs_node_t *diskfs_vfs_finddir(fs_node_t *node, const char *name);
static fs_node_t *diskfs_vfs_create(fs_node_t *node, const char *name);
static int diskfs_vfs_unlink(fs_node_t *node, const char *name);
static int diskfs_vfs_mkdir(fs_node_t *node, const char *name);
static int diskfs_vfs_rmdir(fs_node_t *node, const char *name);

static uint32_t diskfs_checksum(const diskfs_superblock_t *superblock) {
    return DISKFS_CHECKSUM_SEED ^
           superblock->magic ^
           superblock->version ^
           superblock->generation ^
           superblock->total_sectors ^
           superblock->directory_lba ^
           superblock->directory_sectors ^
           superblock->data_lba ^
           superblock->max_files ^
           superblock->sectors_per_file;
}

static int diskfs_name_valid(const char *name) {
    if (!name) return 0;

    for (uint32_t length = 0; length < DISKFS_FILE_NAME_SIZE; length++) {
        if (name[length] == '\0') return length > 0;
        if (name[length] == '/') return 0;
    }
    return 0;
}

static int diskfs_find_child(uint8_t parent, const char *name) {
    if (!diskfs_name_valid(name)) return -1;

    for (uint32_t i = 0; i < DISKFS_MAX_FILES; i++) {
        if (diskfs_entries[i].used && diskfs_entries[i].parent == parent &&
            strcmp(diskfs_entries[i].name, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int diskfs_find_file(const char *name) {
    int index = diskfs_find_child(DISKFS_ROOT_PARENT, name);

    return index >= 0 && diskfs_entries[index].type == DISKFS_ENTRY_FILE ?
           index : -1;
}

static int diskfs_entry_has_children(uint32_t index) {
    for (uint32_t i = 0; i < DISKFS_MAX_FILES; i++) {
        if (diskfs_entries[i].used && diskfs_entries[i].parent == index)
            return 1;
    }
    return 0;
}

static int diskfs_parent_chain_valid(uint32_t index) {
    uint8_t parent = diskfs_entries[index].parent;

    for (uint32_t depth = 0; depth < DISKFS_MAX_FILES; depth++) {
        if (parent == DISKFS_ROOT_PARENT) return 1;
        if (parent >= DISKFS_MAX_FILES || parent == index ||
            !diskfs_entries[parent].used ||
            diskfs_entries[parent].type != DISKFS_ENTRY_DIRECTORY) {
            return 0;
        }
        parent = diskfs_entries[parent].parent;
    }
    return 0;
}

static int diskfs_superblock_valid(const diskfs_superblock_t *superblock) {
    uint32_t required_sectors =
        DISKFS_DATA_LBA + DISKFS_MAX_FILES * DISKFS_FILE_SECTORS;

    return superblock->magic == DISKFS_MAGIC &&
           superblock->version == DISKFS_VERSION &&
           superblock->total_sectors == ata_get_sector_count() &&
           superblock->directory_lba == DISKFS_DIRECTORY_LBA &&
           superblock->directory_sectors == DISKFS_DIRECTORY_SECTORS &&
           superblock->data_lba == DISKFS_DATA_LBA &&
           superblock->max_files == DISKFS_MAX_FILES &&
           superblock->sectors_per_file == DISKFS_FILE_SECTORS &&
           superblock->total_sectors >= required_sectors &&
           superblock->checksum == diskfs_checksum(superblock);
}

static int diskfs_entries_valid(void) {
    const uint32_t max_size = DISKFS_FILE_SECTORS * ATA_SECTOR_SIZE;

    for (uint32_t i = 0; i < DISKFS_MAX_FILES; i++) {
        if (!diskfs_entries[i].used) continue;
        if (diskfs_entries[i].used != 1 ||
            !diskfs_name_valid(diskfs_entries[i].name) ||
            (diskfs_entries[i].type != DISKFS_ENTRY_FILE &&
             diskfs_entries[i].type != DISKFS_ENTRY_DIRECTORY) ||
            (diskfs_entries[i].type == DISKFS_ENTRY_FILE &&
             diskfs_entries[i].length > max_size) ||
            (diskfs_entries[i].type == DISKFS_ENTRY_DIRECTORY &&
             diskfs_entries[i].length != 0) ||
            !diskfs_parent_chain_valid(i)) {
            return 0;
        }

        for (uint32_t j = i + 1; j < DISKFS_MAX_FILES; j++) {
            if (diskfs_entries[j].used &&
                diskfs_entries[i].parent == diskfs_entries[j].parent &&
                strcmp(diskfs_entries[i].name, diskfs_entries[j].name) == 0) {
                return 0;
            }
        }
    }
    return 1;
}

static int diskfs_has_open_files(void) {
    for (uint32_t i = 0; i < DISKFS_MAX_FILES; i++) {
        if (diskfs_open_refs[i] != 0) return 1;
    }
    return 0;
}

static void diskfs_refresh_node(uint32_t index) {
    fs_node_t *node = &diskfs_nodes[index];

    memset(node, 0, sizeof(*node));
    if (!diskfs_entries[index].used) return;

    strcpy(node->name, diskfs_entries[index].name);
    node->inode = index + 1;
    node->length = diskfs_entries[index].length;
    node->impl = index;
    if (diskfs_entries[index].type == DISKFS_ENTRY_DIRECTORY) {
        node->flags = FS_DIRECTORY;
        node->readdir = diskfs_vfs_readdir;
        node->finddir = diskfs_vfs_finddir;
        node->create = diskfs_vfs_create;
        node->unlink = diskfs_vfs_unlink;
        node->mkdir = diskfs_vfs_mkdir;
        node->rmdir = diskfs_vfs_rmdir;
    } else {
        node->flags = FS_FILE;
        node->read = diskfs_vfs_read;
        node->write = diskfs_vfs_write;
        node->open = diskfs_vfs_open;
        node->close = diskfs_vfs_close;
    }
}

static void diskfs_refresh_nodes(void) {
    for (uint32_t i = 0; i < DISKFS_MAX_FILES; i++) {
        diskfs_refresh_node(i);
    }
}

static void diskfs_init_root_node(void) {
    memset(&diskfs_root_node, 0, sizeof(diskfs_root_node));
    strcpy(diskfs_root_node.name, "disk");
    diskfs_root_node.flags = FS_DIRECTORY;
    diskfs_root_node.readdir = diskfs_vfs_readdir;
    diskfs_root_node.finddir = diskfs_vfs_finddir;
    diskfs_root_node.create = diskfs_vfs_create;
    diskfs_root_node.unlink = diskfs_vfs_unlink;
    diskfs_root_node.mkdir = diskfs_vfs_mkdir;
    diskfs_root_node.rmdir = diskfs_vfs_rmdir;
}

static int diskfs_read_directory(void) {
    uint8_t sector[ATA_SECTOR_SIZE];
    uint8_t *entries = (uint8_t *)diskfs_entries;
    uint32_t remaining = sizeof(diskfs_entries);

    memset(diskfs_entries, 0, sizeof(diskfs_entries));
    for (uint32_t i = 0; i < DISKFS_DIRECTORY_SECTORS; i++) {
        uint32_t count = remaining > ATA_SECTOR_SIZE ?
                         ATA_SECTOR_SIZE : remaining;

        if (!ata_read_sector(DISKFS_DIRECTORY_LBA + i, sector)) return 0;
        memcpy(entries + i * ATA_SECTOR_SIZE, sector, count);
        remaining -= count;
    }
    return diskfs_entries_valid();
}

static int diskfs_write_directory(void) {
    uint8_t sector[ATA_SECTOR_SIZE];
    const uint8_t *entries = (const uint8_t *)diskfs_entries;
    uint32_t remaining = sizeof(diskfs_entries);

    for (uint32_t i = 0; i < DISKFS_DIRECTORY_SECTORS; i++) {
        uint32_t count = remaining > ATA_SECTOR_SIZE ?
                         ATA_SECTOR_SIZE : remaining;

        memset(sector, 0, sizeof(sector));
        memcpy(sector, entries + i * ATA_SECTOR_SIZE, count);
        if (!ata_write_sector(DISKFS_DIRECTORY_LBA + i, sector)) return 0;
        remaining -= count;
    }
    return 1;
}

static int diskfs_write_superblock(void) {
    diskfs_superblock.checksum = diskfs_checksum(&diskfs_superblock);
    return ata_write_sector(DISKFS_SUPERBLOCK_LBA,
                            (const uint8_t *)&diskfs_superblock);
}

static int diskfs_store_metadata(int increment_generation) {
    if (!diskfs_mounted) return 0;
    if (increment_generation) diskfs_superblock.generation++;

    if (!diskfs_write_directory() || !diskfs_write_superblock()) {
        diskfs_mounted = 0;
        return 0;
    }
    return 1;
}

void diskfs_install(void) {
    diskfs_mounted = 0;
    memset(&diskfs_superblock, 0, sizeof(diskfs_superblock));
    memset(diskfs_entries, 0, sizeof(diskfs_entries));
    memset(diskfs_open_refs, 0, sizeof(diskfs_open_refs));
    diskfs_init_root_node();
    diskfs_refresh_nodes();
    if (ata_is_available()) diskfs_mount();
}

int diskfs_format(void) {
    if (!ata_is_available() || diskfs_has_open_files() ||
        ata_get_sector_count() <
        DISKFS_DATA_LBA + DISKFS_MAX_FILES * DISKFS_FILE_SECTORS) {
        return 0;
    }

    memset(&diskfs_superblock, 0, sizeof(diskfs_superblock));
    memset(diskfs_entries, 0, sizeof(diskfs_entries));
    diskfs_superblock.magic = DISKFS_MAGIC;
    diskfs_superblock.version = DISKFS_VERSION;
    diskfs_superblock.generation = 1;
    diskfs_superblock.total_sectors = ata_get_sector_count();
    diskfs_superblock.directory_lba = DISKFS_DIRECTORY_LBA;
    diskfs_superblock.directory_sectors = DISKFS_DIRECTORY_SECTORS;
    diskfs_superblock.data_lba = DISKFS_DATA_LBA;
    diskfs_superblock.max_files = DISKFS_MAX_FILES;
    diskfs_superblock.sectors_per_file = DISKFS_FILE_SECTORS;
    diskfs_mounted = 1;

    if (!diskfs_write_directory() || !diskfs_write_superblock()) {
        diskfs_mounted = 0;
        return 0;
    }
    diskfs_refresh_nodes();
    return 1;
}

int diskfs_mount(void) {
    diskfs_superblock_t superblock;

    if (!ata_is_available() || diskfs_has_open_files()) return 0;

    diskfs_mounted = 0;
    if (
        !ata_read_sector(DISKFS_SUPERBLOCK_LBA, (uint8_t *)&superblock) ||
        !diskfs_superblock_valid(&superblock)) {
        return 0;
    }

    diskfs_superblock = superblock;
    if (!diskfs_read_directory()) return 0;
    diskfs_mounted = 1;
    diskfs_refresh_nodes();
    return 1;
}

static int diskfs_create_entry(uint8_t parent, const char *name, uint8_t type) {
    if (!diskfs_mounted || !diskfs_name_valid(name) ||
        (type != DISKFS_ENTRY_FILE && type != DISKFS_ENTRY_DIRECTORY) ||
        diskfs_find_child(parent, name) >= 0 ||
        (parent != DISKFS_ROOT_PARENT &&
         (parent >= DISKFS_MAX_FILES || !diskfs_entries[parent].used ||
          diskfs_entries[parent].type != DISKFS_ENTRY_DIRECTORY))) {
        return -1;
    }

    for (uint32_t i = 0; i < DISKFS_MAX_FILES; i++) {
        if (!diskfs_entries[i].used) {
            memset(&diskfs_entries[i], 0, sizeof(diskfs_entries[i]));
            diskfs_entries[i].used = 1;
            diskfs_entries[i].type = type;
            diskfs_entries[i].parent = parent;
            strcpy(diskfs_entries[i].name, name);
            if (!diskfs_store_metadata(1)) return -1;
            diskfs_refresh_node(i);
            return (int)i;
        }
    }
    return -1;
}

static int diskfs_remove_entry(uint32_t index, uint8_t type) {
    if (!diskfs_mounted || index >= DISKFS_MAX_FILES ||
        !diskfs_entries[index].used || diskfs_entries[index].type != type ||
        diskfs_open_refs[index] != 0 ||
        (type == DISKFS_ENTRY_DIRECTORY && diskfs_entry_has_children(index))) {
        return 0;
    }

    memset(&diskfs_entries[index], 0, sizeof(diskfs_entries[index]));
    if (!diskfs_store_metadata(1)) return 0;
    diskfs_refresh_node(index);
    return 1;
}

int diskfs_create_file(const char *name) {
    if (diskfs_create_entry(DISKFS_ROOT_PARENT, name, DISKFS_ENTRY_FILE) >= 0)
        return 1;
    return 0;
}

int diskfs_write_file(const char *name, const uint8_t *buffer, uint32_t size) {
    uint8_t sector[ATA_SECTOR_SIZE];
    uint32_t written = 0;
    int index = diskfs_find_file(name);

    if (!diskfs_mounted || index < 0 ||
        diskfs_entries[index].type != DISKFS_ENTRY_FILE ||
        (!buffer && size != 0) ||
        size > DISKFS_FILE_SECTORS * ATA_SECTOR_SIZE) {
        return 0;
    }

    for (uint32_t i = 0; written < size; i++) {
        uint32_t count = size - written > ATA_SECTOR_SIZE ?
                         ATA_SECTOR_SIZE : size - written;

        memset(sector, 0, sizeof(sector));
        memcpy(sector, buffer + written, count);
        if (!ata_write_sector(DISKFS_DATA_LBA +
                              (uint32_t)index * DISKFS_FILE_SECTORS + i,
                              sector)) {
            return 0;
        }
        written += count;
    }

    diskfs_entries[index].length = size;
    if (!diskfs_store_metadata(1)) return 0;
    diskfs_refresh_node((uint32_t)index);
    return 1;
}

int diskfs_read_file(const char *name, uint8_t *buffer, uint32_t size) {
    uint8_t sector[ATA_SECTOR_SIZE];
    uint32_t copied = 0;
    int index = diskfs_find_file(name);

    if (!diskfs_mounted || index < 0 ||
        diskfs_entries[index].type != DISKFS_ENTRY_FILE || !buffer) {
        return -1;
    }
    if (size > diskfs_entries[index].length) size = diskfs_entries[index].length;

    for (uint32_t i = 0; copied < size; i++) {
        uint32_t count = size - copied > ATA_SECTOR_SIZE ?
                         ATA_SECTOR_SIZE : size - copied;

        if (!ata_read_sector(DISKFS_DATA_LBA +
                             (uint32_t)index * DISKFS_FILE_SECTORS + i,
                             sector)) {
            return -1;
        }
        memcpy(buffer + copied, sector, count);
        copied += count;
    }
    return (int)copied;
}

int diskfs_unlink_file(const char *name) {
    int index = diskfs_find_file(name);

    return index >= 0 ?
           diskfs_remove_entry((uint32_t)index, DISKFS_ENTRY_FILE) : 0;
}

int diskfs_is_mounted(void) {
    return diskfs_mounted;
}

uint32_t diskfs_get_generation(void) {
    return diskfs_mounted ? diskfs_superblock.generation : 0;
}

uint32_t diskfs_get_file_count(void) {
    uint32_t count = 0;

    if (!diskfs_mounted) return 0;
    for (uint32_t i = 0; i < DISKFS_MAX_FILES; i++) {
        if (diskfs_entries[i].used &&
            diskfs_entries[i].type == DISKFS_ENTRY_FILE) {
            count++;
        }
    }
    return count;
}

static int diskfs_read_slot(uint32_t index, uint32_t offset,
                            uint8_t *buffer, uint32_t size) {
    uint8_t sector[ATA_SECTOR_SIZE];
    uint32_t copied = 0;

    if (index >= DISKFS_MAX_FILES || !diskfs_entries[index].used ||
        diskfs_entries[index].type != DISKFS_ENTRY_FILE || !buffer)
        return -1;
    if (offset >= diskfs_entries[index].length) return 0;
    if (size > diskfs_entries[index].length - offset)
        size = diskfs_entries[index].length - offset;

    while (copied < size) {
        uint32_t position = offset + copied;
        uint32_t sector_offset = position % ATA_SECTOR_SIZE;
        uint32_t count = ATA_SECTOR_SIZE - sector_offset;

        if (count > size - copied) count = size - copied;
        if (!ata_read_sector(DISKFS_DATA_LBA +
                             index * DISKFS_FILE_SECTORS +
                             position / ATA_SECTOR_SIZE,
                             sector)) {
            return -1;
        }
        memcpy(buffer + copied, sector + sector_offset, count);
        copied += count;
    }
    return (int)copied;
}

static int diskfs_write_slot(uint32_t index, uint32_t offset,
                             const uint8_t *buffer, uint32_t size) {
    uint8_t sector[ATA_SECTOR_SIZE];
    uint32_t old_length;
    uint32_t new_length;
    uint32_t first_sector;
    uint32_t last_sector;

    if (index >= DISKFS_MAX_FILES || !diskfs_entries[index].used ||
        diskfs_entries[index].type != DISKFS_ENTRY_FILE ||
        (!buffer && size != 0) ||
        offset > DISKFS_FILE_SECTORS * ATA_SECTOR_SIZE ||
        size > DISKFS_FILE_SECTORS * ATA_SECTOR_SIZE - offset) {
        return 0;
    }
    if (size == 0) return 1;

    old_length = diskfs_entries[index].length;
    new_length = offset + size > old_length ? offset + size : old_length;
    first_sector = (old_length < offset ? old_length : offset) / ATA_SECTOR_SIZE;
    last_sector = (offset + size - 1) / ATA_SECTOR_SIZE;

    for (uint32_t current = first_sector; current <= last_sector; current++) {
        uint32_t sector_start = current * ATA_SECTOR_SIZE;
        uint32_t sector_end = sector_start + ATA_SECTOR_SIZE;
        uint32_t zero_start = old_length > sector_start ? old_length : sector_start;
        uint32_t zero_end = offset < sector_end ? offset : sector_end;
        uint32_t copy_start = offset > sector_start ? offset : sector_start;
        uint32_t copy_end = offset + size < sector_end ?
                            offset + size : sector_end;

        if (sector_start < old_length) {
            if (!ata_read_sector(DISKFS_DATA_LBA +
                                 index * DISKFS_FILE_SECTORS + current,
                                 sector)) {
                return 0;
            }
        } else {
            memset(sector, 0, sizeof(sector));
        }

        if (zero_end > zero_start)
            memset(sector + zero_start - sector_start, 0, zero_end - zero_start);
        if (copy_end > copy_start)
            memcpy(sector + copy_start - sector_start,
                   buffer + copy_start - offset,
                   copy_end - copy_start);

        if (!ata_write_sector(DISKFS_DATA_LBA +
                              index * DISKFS_FILE_SECTORS + current,
                              sector)) {
            return 0;
        }
    }

    diskfs_entries[index].length = new_length;
    if (!diskfs_store_metadata(1)) return 0;
    diskfs_refresh_node(index);
    return 1;
}

static uint32_t diskfs_vfs_read(fs_node_t *node, uint32_t offset,
                                uint32_t size, uint8_t *buffer) {
    int result;

    if (!node) return 0;
    result = diskfs_read_slot(node->impl, offset, buffer, size);
    return result < 0 ? 0 : (uint32_t)result;
}

static uint32_t diskfs_vfs_write(fs_node_t *node, uint32_t offset,
                                 uint32_t size, uint8_t *buffer) {
    if (!node || !diskfs_write_slot(node->impl, offset, buffer, size)) return 0;
    return size;
}

static void diskfs_vfs_open(fs_node_t *node) {
    if (node && node->impl < DISKFS_MAX_FILES)
        diskfs_open_refs[node->impl]++;
}

static void diskfs_vfs_close(fs_node_t *node) {
    if (node && node->impl < DISKFS_MAX_FILES &&
        diskfs_open_refs[node->impl] != 0) {
        diskfs_open_refs[node->impl]--;
    }
}

static int diskfs_vfs_parent(fs_node_t *node, uint8_t *parent) {
    if (node == &diskfs_root_node) {
        *parent = DISKFS_ROOT_PARENT;
        return 1;
    }

    for (uint32_t i = 0; i < DISKFS_MAX_FILES; i++) {
        if (node == &diskfs_nodes[i] && diskfs_entries[i].used &&
            diskfs_entries[i].type == DISKFS_ENTRY_DIRECTORY) {
            *parent = (uint8_t)i;
            return 1;
        }
    }
    return 0;
}

static dirent_t *diskfs_vfs_readdir(fs_node_t *node, uint32_t index) {
    uint32_t child_index = 0;
    uint8_t parent;

    if (!diskfs_mounted || !diskfs_vfs_parent(node, &parent)) return NULL;

    for (uint32_t i = 0; i < DISKFS_MAX_FILES; i++) {
        if (!diskfs_entries[i].used || diskfs_entries[i].parent != parent)
            continue;
        if (child_index++ != index) continue;

        strcpy(diskfs_dirent.name, diskfs_entries[i].name);
        diskfs_dirent.inode = i + 1;
        return &diskfs_dirent;
    }
    return NULL;
}

static fs_node_t *diskfs_vfs_finddir(fs_node_t *node, const char *name) {
    int index;
    uint8_t parent;

    if (!diskfs_mounted || !diskfs_vfs_parent(node, &parent)) return NULL;
    index = diskfs_find_child(parent, name);
    return index < 0 ? NULL : &diskfs_nodes[index];
}

static fs_node_t *diskfs_vfs_create(fs_node_t *node, const char *name) {
    int index;
    uint8_t parent;

    if (!diskfs_mounted || !diskfs_vfs_parent(node, &parent)) return NULL;
    index = diskfs_create_entry(parent, name, DISKFS_ENTRY_FILE);
    return index < 0 ? NULL : &diskfs_nodes[index];
}

static int diskfs_vfs_unlink(fs_node_t *node, const char *name) {
    int index;
    uint8_t parent;

    if (!diskfs_mounted || !diskfs_vfs_parent(node, &parent)) return -1;
    index = diskfs_find_child(parent, name);
    return index >= 0 &&
           diskfs_remove_entry((uint32_t)index, DISKFS_ENTRY_FILE) ? 0 : -1;
}

static int diskfs_vfs_mkdir(fs_node_t *node, const char *name) {
    uint8_t parent;

    if (!diskfs_mounted || !diskfs_vfs_parent(node, &parent)) return -1;
    return diskfs_create_entry(parent, name, DISKFS_ENTRY_DIRECTORY) >= 0 ?
           0 : -1;
}

static int diskfs_vfs_rmdir(fs_node_t *node, const char *name) {
    int index;
    uint8_t parent;

    if (!diskfs_mounted || !diskfs_vfs_parent(node, &parent)) return -1;
    index = diskfs_find_child(parent, name);
    return index >= 0 &&
           diskfs_remove_entry((uint32_t)index, DISKFS_ENTRY_DIRECTORY) ?
           0 : -1;
}

fs_node_t *diskfs_get_root_node(void) {
    return diskfs_mounted ? &diskfs_root_node : NULL;
}
