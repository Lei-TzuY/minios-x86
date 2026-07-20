#ifndef DISKFS_H
#define DISKFS_H

#include <stdint.h>
#include "fs.h"

void diskfs_install(void);
int diskfs_format(void);
int diskfs_mount(void);
int diskfs_create_file(const char *name);
int diskfs_write_file(const char *name, const uint8_t *buffer, uint32_t size);
int diskfs_read_file(const char *name, uint8_t *buffer, uint32_t size);
int diskfs_unlink_file(const char *name);
int diskfs_is_mounted(void);
uint32_t diskfs_get_generation(void);
uint32_t diskfs_get_file_count(void);
fs_node_t *diskfs_get_root_node(void);

#endif
