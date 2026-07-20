#ifndef FS_H
#define FS_H

#include <stdint.h>
#include <stddef.h>

#define FS_FILE        0x01
#define FS_DIRECTORY   0x02
#define FS_CHARDEVICE  0x03
#define FS_BLOCKDEVICE 0x04
#define FS_PIPE        0x05
#define FS_SYMLINK     0x06
#define FS_MOUNTPOINT  0x08 

struct fs_node;

/* Function pointer definitions for VFS operations */
typedef uint32_t (*read_type_t)(struct fs_node*, uint32_t, uint32_t, uint8_t*);
typedef uint32_t (*write_type_t)(struct fs_node*, uint32_t, uint32_t, uint8_t*);
typedef void (*open_type_t)(struct fs_node*);
typedef void (*close_type_t)(struct fs_node*);

typedef struct dirent {
    char name[128];
    uint32_t inode;
} dirent_t;

typedef dirent_t *(*readdir_type_t)(struct fs_node*, uint32_t);
typedef struct fs_node *(*finddir_type_t)(struct fs_node*, const char*);
typedef struct fs_node *(*create_type_t)(struct fs_node*, const char*);
typedef int (*unlink_type_t)(struct fs_node*, const char*);
typedef int (*mkdir_type_t)(struct fs_node*, const char*);
typedef int (*rmdir_type_t)(struct fs_node*, const char*);

typedef struct fs_node {
    char name[128];     // The requested name.
    uint32_t mask;      // The permissions mask.
    uint32_t uid;       // The owning user.
    uint32_t gid;       // The owning group.
    uint32_t flags;     // Includes the node type.
    uint32_t inode;     // This is device-specific.
    uint32_t length;    // Size of the file, in bytes.
    uint32_t impl;      // An implementation-defined number.
    read_type_t read;
    write_type_t write;
    open_type_t open;
    close_type_t close;
    readdir_type_t readdir;
    finddir_type_t finddir;
    create_type_t create;
    unlink_type_t unlink;
    mkdir_type_t mkdir;
    rmdir_type_t rmdir;
    void* ptr;          // Used by mountpoints and symlinks.
} fs_node_t;

#define FS_MAX_PATH 128

/* Global root filesystem */
extern fs_node_t *fs_root;

uint32_t read_fs(fs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer);
uint32_t write_fs(fs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer);
void open_fs(fs_node_t *node);
void close_fs(fs_node_t *node);
dirent_t *readdir_fs(fs_node_t *node, uint32_t index);
fs_node_t *finddir_fs(fs_node_t *node, const char *name);
fs_node_t *resolve_fs(const char *path);
fs_node_t *create_fs(const char *path);

/* Combine a working directory and a (possibly relative) path into a clean
 * absolute path with '.'/'..' resolved. `out` must hold FS_MAX_PATH bytes.
 * Returns 0 on success, -1 if the result would overflow. */
int vfs_resolve_path(const char *cwd, const char *path, char *out);
int unlink_fs(const char *path);
int mkdir_fs(const char *path);
int rmdir_fs(const char *path);

#endif
