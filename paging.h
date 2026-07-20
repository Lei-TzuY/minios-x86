#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

#define PAGE_PRESENT  0x01
#define PAGE_RW       0x02
#define PAGE_USER     0x04

typedef struct page_entry {
    uint32_t present    : 1;
    uint32_t rw         : 1;
    uint32_t user       : 1;
    uint32_t accessed   : 1;
    uint32_t dirty      : 1;
    /* Bits 5-10 hold hardware-managed flags (real accessed/dirty/PAT/global) and
     * are ignored by software. `shared` must live in a truly OS-available bit
     * (9-11) that hardware never writes; bit 5 would be clobbered by the CPU's
     * accessed flag, falsely marking every touched page as shared. */
    uint32_t unused     : 6;   /* bits 5-10 */
    uint32_t shared     : 1;   /* bit 11: shared memory (fork shares, no COW) */
    uint32_t frame      : 20;
} __attribute__((packed)) page_entry_t;

typedef struct page_table {
    page_entry_t pages[1024];
} page_table_t;

typedef struct page_directory {
    /* Pointers to page tables (physical addresses) */
    uint32_t tablesPhysical[1024];
} page_directory_t;

typedef struct address_space {
    page_directory_t *directory;
    page_table_t *first_table;   /* private mapping for the low 0-4MB window */
    page_table_t *ext_table;     /* private mapping for the mmap region (32-36MB) */
    uint8_t owns_tables;
} address_space_t;

void paging_install(void);
void enable_paging(uint32_t directory_address);
address_space_t *paging_get_kernel_address_space(void);
address_space_t *paging_create_user_address_space(void);
/* Deep-copy a user address space: every mapped user page is duplicated into a
 * freshly allocated frame at the same virtual address (eager fork). */
address_space_t *paging_clone_address_space(address_space_t *parent);
void paging_destroy_user_address_space(address_space_t *space);
void paging_activate_address_space(address_space_t *space);
int paging_map_user_page(address_space_t *space, uint32_t vaddr);
/* Unmap one user page: release its frame (respecting COW sharing) and clear
 * the PTE so later touches fault. A never-demand-mapped page is a no-op. */
void paging_unmap_user_page(address_space_t *space, uint32_t vaddr);
/* Map a zero-filled, writable page at `vaddr` in the active space and mark it
 * shared: fork maps the same frame into the child (writable, no copy-on-write)
 * so both processes see each other's writes. Returns 1 on success. */
int paging_share_page(uint32_t vaddr);
int paging_zero_user(address_space_t *space, uint32_t vaddr, uint32_t size);
int paging_copy_to_user(address_space_t *space, uint32_t vaddr,
                        const uint8_t *src, uint32_t size);
int paging_user_range_mapped(uint32_t vaddr, uint32_t size);
uint32_t paging_get_user_page_count(void);
uint32_t paging_get_user_space_count(void);

#endif
