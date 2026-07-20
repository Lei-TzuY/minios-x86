#ifndef PROCFS_H
#define PROCFS_H

#include "fs.h"

/* A read-only synthetic filesystem exposing kernel/process state as files.
 * Mount the returned directory node with ramfs_mount("proc", procfs_get_root()).
 * Contents are generated on each read, so they always reflect live state:
 *   /proc/count      -> number of live processes
 *   /proc/uptime     -> timer ticks since boot (100 Hz)
 *   /proc/processes  -> one "pid ppid state name" line per process
 */
fs_node_t *procfs_get_root(void);

#endif
