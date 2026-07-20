#ifndef ATA_H
#define ATA_H

#include <stdint.h>

#define ATA_SECTOR_SIZE 512

void ata_install(void);
int ata_read_sector(uint32_t lba, uint8_t *buffer);
int ata_write_sector(uint32_t lba, const uint8_t *buffer);
int ata_is_available(void);
uint32_t ata_get_sector_count(void);
uint32_t ata_get_read_count(void);
uint32_t ata_get_write_count(void);

#endif
