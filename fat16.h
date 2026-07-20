#ifndef FAT16_H
#define FAT16_H

#include <stdint.h>
#include "fs.h"

/* Mount a read-only FAT16 image (in memory). Safe to call once at boot. */
void fat16_install(const uint8_t *image, uint32_t size);
int fat16_is_mounted(void);
fs_node_t *fat16_get_root_node(void);

#endif
