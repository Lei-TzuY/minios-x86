#ifndef PMM_H
#define PMM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#define PMM_BLOCK_SIZE 4096
#define PMM_MAX_MEMORY (32U * 1024U * 1024U)

void pmm_init(uint32_t mem_size);
void pmm_init_region(uint32_t base, uint32_t size);
void pmm_deinit_region(uint32_t base, uint32_t size);
void* pmm_alloc_block(void);
void* pmm_alloc_blocks(uint32_t count);
void pmm_free_block(void* p);
void pmm_free_blocks(void* p, uint32_t count);
uint32_t pmm_get_total_blocks(void);
uint32_t pmm_get_used_blocks(void);
uint32_t pmm_get_free_blocks(void);

#endif
