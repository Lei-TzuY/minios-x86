#ifndef RAMFS_H
#define RAMFS_H

#include "fs.h"

void ramfs_init(void);
fs_node_t *ramfs_create_file(const char *path);
fs_node_t *ramfs_create_static_file(const char *path, const uint8_t *data, uint32_t size);
fs_node_t *ramfs_find_node(const char *path);
fs_node_t *ramfs_find_file(const char *path);
int ramfs_unlink_file(const char *path);
int ramfs_create_directory(const char *path);
int ramfs_remove_directory(const char *path);
int ramfs_mount(const char *path, fs_node_t *node);
uint32_t ramfs_get_node_count(void);

#endif
