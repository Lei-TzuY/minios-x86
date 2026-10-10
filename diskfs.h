#ifndef DISKFS_H
#define DISKFS_H

#include <stdint.h>
#include "fs.h"

/* Task-context API; never call from an IRQ handler. Operations serialize on
 * this volume and may wait. install is boot initialization (before clients).
 * ATA must return through these calls on failure, not terminate their task.
 *
 * VFS nodes/dirents are borrowed results. Keep lookup-to-open and dirent copy
 * free of scheduling points; hold an open reference across later node use.
 * Reference callbacks never sleep. Reads/writes also pin their node in flight.
 * A failed write takes the volume offline; close references before remounting.
 * Serialization is not crash atomicity: failed sectors cannot be rolled back.
 * See docs/DISKFS_SERIALIZATION.md for the integration boundaries. */
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
