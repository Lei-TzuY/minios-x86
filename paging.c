#include "paging.h"
#include "heap.h"
#include "pmm.h"
#include "isr.h"
#include "task.h"
#include "vga.h"
#include "utils.h"
#include "elf_loader.h"   /* USER_STACK_BOTTOM / USER_STACK_TOP */
#include "process.h"      /* current process heap bounds */

/* All paging structures are physical addresses in the current identity map. */
#define KERNEL_IDENTITY_TABLES (PMM_MAX_MEMORY / (1024U * PMM_BLOCK_SIZE))

static page_directory_t kernel_directory __attribute__((aligned(4096)));
static page_table_t kernel_tables[KERNEL_IDENTITY_TABLES]
    __attribute__((aligned(4096)));
static address_space_t kernel_space;
static address_space_t *active_space;
static uint32_t user_space_count;

/* Copy-on-write sharing counts, indexed by physical frame number. The value is
 * the number of EXTRA references beyond the sole owner (0 = owned by one PTE). */
#define FRAME_COUNT (PMM_MAX_MEMORY / PMM_BLOCK_SIZE)
static uint16_t page_ref[FRAME_COUNT];

static void cow_ref_inc(uint32_t frame) {
    if (frame < FRAME_COUNT) page_ref[frame]++;
}

/* Release one reference. Returns 1 if the frame should now be freed. */
static int cow_ref_release(uint32_t frame) {
    if (frame < FRAME_COUNT && page_ref[frame] > 0) {
        page_ref[frame]--;
        return 0;
    }
    return 1;
}

static void load_cr3(page_directory_t *directory);

/* Flush a single page's TLB entry (cheaper than reloading CR3). */
static void paging_invlpg(uint32_t vaddr) {
    __asm__ volatile("invlpg (%0)" :: "r"(vaddr) : "memory");
}

/* Full TLB flush (used when many entries change at once, e.g. fork). */
static void paging_flush_tlb(void) {
    if (active_space) load_cr3(active_space->directory);
}

/* Locate the page-table entry backing a user virtual address, selecting the
 * private low table (0-4MB) or the mmap table (32-36MB). Returns NULL for
 * addresses outside either user region (e.g. the kernel identity gap). */
static page_entry_t *user_pte(address_space_t *space, uint32_t vaddr) {
    if (!space) return NULL;
    if (vaddr < 0x400000U)
        return space->first_table ? &space->first_table->pages[vaddr >> 12] : NULL;
    if (vaddr >= USER_EXT_BASE && vaddr < USER_EXT_TOP)
        return space->ext_table
                   ? &space->ext_table->pages[(vaddr - USER_EXT_BASE) >> 12]
                   : NULL;
    return NULL;
}

/* Page in `addr` for the active user space if it is not already a usable user
 * page (absent, or still the kernel identity mapping). Returns 1 if mapped. */
static int paging_demand_map(uint32_t addr) {
    page_entry_t *page;

    if (!active_space || !active_space->owns_tables) return 0;
    page = user_pte(active_space, addr);
    if (!page) return 0;
    if (page->present && page->user) return 0;   /* already usable */
    if (!paging_map_user_page(active_space, addr & ~0xFFFU)) return 0;

    /* Invalidate any cached kernel-identity translation for this page so the
     * retry uses the fresh user mapping. */
    paging_invlpg(addr & ~0xFFFU);
    return 1;
}

/* Resolve a copy-on-write write fault. Returns 1 if handled. */
static int paging_try_cow(uint32_t addr) {
    page_entry_t *page;
    uint32_t frame;

    page = active_space ? user_pte(active_space, addr) : NULL;
    if (!page || !page->present || !page->user || page->rw) return 0; /* not COW */

    frame = page->frame;
    if (page_ref[frame] > 0) {
        /* Shared: give this space a private writable copy. */
        void *new_frame = pmm_alloc_block();
        if (!new_frame) return 0;
        memcpy(new_frame, (void *)(frame << 12), PMM_BLOCK_SIZE);
        page_ref[frame]--;
        page->frame = (uint32_t)new_frame >> 12;
    }
    /* Sole owner (or just copied): simply re-grant write access. Only this one
     * page changed, so a single-page invalidation suffices. */
    page->rw = 1;
    paging_invlpg(addr & ~0xFFFU);
    return 1;
}

static void page_fault_handler(registers_t *regs) {
    uint32_t faulting_address;
    __asm__ volatile("mov %%cr2, %0" : "=r" (faulting_address));

    int not_present = !(regs->err_code & 0x1);
    int write = regs->err_code & 0x2;
    int user_mode = regs->err_code & 0x4;
    int reserved = regs->err_code & 0x8;

    /* A write to a present read-only user page is copy-on-write (from fork).
     * Handle it for both user and kernel writes (WP makes kernel writes trap). */
    if (write && paging_try_cow(faulting_address)) return;

    /* A not-present access inside the user stack region is paged in on demand
     * (the stack grows down; pages below the eagerly-mapped top fault in here). */
    /* Demand-page the user stack and heap: a fault inside either region on a
     * page that is not yet a usable user page is filled in here. The page may
     * be absent, or still hold the kernel identity mapping (present, user=0),
     * since these regions are not mapped eagerly. (COW write faults on present
     * user pages are handled above.) */
    if (faulting_address >= USER_STACK_BOTTOM &&
        faulting_address < USER_STACK_TOP &&
        paging_demand_map(faulting_address)) {
        return;
    }
    {
        process_t *proc = process_get_current();
        if (proc && faulting_address >= proc->heap_start &&
            faulting_address < proc->heap_break &&
            paging_demand_map(faulting_address)) {
            return;
        }
        /* mmap region (above the kernel identity map): only pages currently
         * reserved by SYS_MMAP are filled on demand, so touching a freed
         * (munmapped) page falls through and faults like any bad pointer. */
        if (proc && process_ext_reserved(proc, faulting_address) &&
            paging_demand_map(faulting_address)) {
            return;
        }
    }

    if ((regs->cs & 0x3) == 0x3) {
        terminal_setcolor(4); /* VGA_COLOR_RED */
        terminal_writestring("\nUSER PAGE FAULT at address ");
        terminal_write_dec(faulting_address);
        terminal_writestring("\nTerminating user program.\n");
        terminal_setcolor(15);
        task_exit(-1);
    }

    terminal_setcolor(4); /* VGA_COLOR_RED */
    terminal_writestring("\nPAGE FAULT! ( ");
    if (not_present) terminal_writestring("not-present ");
    if (write) terminal_writestring("write ");
    if (user_mode) terminal_writestring("user-mode ");
    if (reserved) terminal_writestring("reserved ");
    terminal_writestring(") at address ");
    terminal_write_dec(faulting_address);
    terminal_writestring("\nSystem Halted.\n");

    while (1) { __asm__ volatile("hlt"); }
}

static void load_cr3(page_directory_t *directory) {
    __asm__ volatile("mov %0, %%cr3" :: "r"(directory) : "memory");
}

static uint8_t *user_pointer(address_space_t *space, uint32_t vaddr) {
    page_entry_t *page = user_pte(space, vaddr);

    if (!page || !page->present || !page->user) return NULL;

    return (uint8_t *)((page->frame << 12) | (vaddr & 0xFFFU));
}

void paging_install(void) {
    memset(&kernel_directory, 0, sizeof(kernel_directory));

    /* Keep the first 32MB identity-mapped for kernel heap, task stacks and
     * paging structures allocated from the physical memory manager. */
    for (uint32_t table = 0; table < KERNEL_IDENTITY_TABLES; table++) {
        for (uint32_t page = 0; page < 1024; page++) {
            uint32_t frame = table * 1024 + page;
            kernel_tables[table].pages[page].frame = frame;
            kernel_tables[table].pages[page].present = 1;
            kernel_tables[table].pages[page].rw = 1;
            kernel_tables[table].pages[page].user = 0;
        }
        kernel_directory.tablesPhysical[table] =
            ((uint32_t)&kernel_tables[table]) | PAGE_PRESENT | PAGE_RW;
    }

    kernel_space.directory = &kernel_directory;
    kernel_space.first_table = &kernel_tables[0];
    kernel_space.owns_tables = 0;
    active_space = &kernel_space;

    register_interrupt_handler(14, page_fault_handler);
    enable_paging((uint32_t)&kernel_directory);
}

address_space_t *paging_get_kernel_address_space(void) {
    return &kernel_space;
}

address_space_t *paging_create_user_address_space(void) {
    address_space_t *space = (address_space_t *)kmalloc(sizeof(address_space_t));

    if (!space) return NULL;

    space->directory = (page_directory_t *)pmm_alloc_block();
    space->first_table = (page_table_t *)pmm_alloc_block();
    space->ext_table = (page_table_t *)pmm_alloc_block();
    space->owns_tables = 1;

    if (!space->directory || !space->first_table || !space->ext_table) {
        if (space->directory) pmm_free_block(space->directory);
        if (space->first_table) pmm_free_block(space->first_table);
        if (space->ext_table) pmm_free_block(space->ext_table);
        kfree(space);
        return NULL;
    }

    memcpy(space->directory, &kernel_directory, sizeof(page_directory_t));
    memcpy(space->first_table, &kernel_tables[0], sizeof(page_table_t));
    space->directory->tablesPhysical[0] =
        ((uint32_t)space->first_table) | PAGE_PRESENT | PAGE_RW | PAGE_USER;

    /* Empty private table for the on-demand mmap region (32-36MB). */
    memset(space->ext_table, 0, sizeof(page_table_t));
    space->directory->tablesPhysical[USER_EXT_DIR] =
        ((uint32_t)space->ext_table) | PAGE_PRESENT | PAGE_RW | PAGE_USER;
    user_space_count++;
    return space;
}

/* Copy-on-write clone every mapped user page of one table into another: shared
 * pages keep the same writable frame, ordinary pages become read-only in both
 * spaces (the first write faults and gets a private copy in paging_try_cow). */
static void paging_cow_clone_table(page_table_t *src_t, page_table_t *dst_t) {
    if (!src_t || !dst_t) return;

    for (int i = 0; i < 1024; i++) {
        page_entry_t *src = &src_t->pages[i];
        page_entry_t *dst = &dst_t->pages[i];

        if (!src->present || !src->user) continue;

        if (src->shared) {
            *dst = *src;        /* keeps rw=1 and shared=1 on the same frame */
            cow_ref_inc(src->frame);
            continue;
        }

        src->rw = 0;            /* parent now read-only too */
        *dst = *src;            /* child shares the frame, also read-only */
        cow_ref_inc(src->frame);
    }
}

address_space_t *paging_clone_address_space(address_space_t *parent) {
    address_space_t *child;

    if (!parent || !parent->first_table) return NULL;

    child = paging_create_user_address_space();
    if (!child) return NULL;

    paging_cow_clone_table(parent->first_table, child->first_table);
    paging_cow_clone_table(parent->ext_table, child->ext_table);

    /* The parent is the active space; flush its stale writable TLB entries. */
    paging_flush_tlb();
    return child;
}

void paging_destroy_user_address_space(address_space_t *space) {
    if (!space || !space->owns_tables) return;

    if (space == active_space) {
        paging_activate_address_space(&kernel_space);
    }

    page_table_t *tables[2] = { space->first_table, space->ext_table };
    for (int t = 0; t < 2; t++) {
        if (!tables[t]) continue;
        for (int i = 0; i < 1024; i++) {
            page_entry_t *page = &tables[t]->pages[i];
            if (page->present && page->user) {
                /* Free the frame only once its last COW sharer is gone. */
                if (cow_ref_release(page->frame)) {
                    pmm_free_block((void *)(page->frame << 12));
                }
                page->user = 0;
            }
        }
    }

    pmm_free_block(space->first_table);
    if (space->ext_table) pmm_free_block(space->ext_table);
    pmm_free_block(space->directory);
    kfree(space);
    user_space_count--;
}

void paging_activate_address_space(address_space_t *space) {
    if (!space) space = &kernel_space;
    if (space == active_space) return;

    active_space = space;
    load_cr3(space->directory);
}

int paging_map_user_page(address_space_t *space, uint32_t vaddr) {
    page_entry_t *page;
    void *frame;

    if (!space || !space->owns_tables) return 0;

    page = user_pte(space, vaddr);
    if (!page) return 0;
    if (page->present && page->user) return 1;

    frame = pmm_alloc_block();
    if (!frame) return 0;

    memset(frame, 0, PMM_BLOCK_SIZE);
    page->frame = (uint32_t)frame >> 12;
    page->present = 1;
    page->rw = 1;
    page->user = 1;
    page->accessed = 0;
    page->dirty = 0;
    page->shared = 0;
    return 1;
}

void paging_unmap_user_page(address_space_t *space, uint32_t vaddr) {
    page_entry_t *page;

    if (!space || !space->owns_tables) return;
    page = user_pte(space, vaddr);
    if (!page || !page->present || !page->user) return;   /* never demand-mapped */

    /* Free the frame only once its last COW sharer is gone (as in destroy). */
    if (cow_ref_release(page->frame)) {
        pmm_free_block((void *)(page->frame << 12));
    }
    memset(page, 0, sizeof(*page));
    if (space == active_space) paging_invlpg(vaddr & ~0xFFFU);
}

int paging_share_page(uint32_t vaddr) {
    page_entry_t *page;
    void *frame;

    if (!active_space || !active_space->owns_tables || vaddr >= 0x400000U) return 0;

    page = &active_space->first_table->pages[vaddr >> 12];
    if (page->present && page->user) return page->shared ? 1 : 0;  /* already used */

    frame = pmm_alloc_block();
    if (!frame) return 0;

    memset(frame, 0, PMM_BLOCK_SIZE);
    page->frame = (uint32_t)frame >> 12;
    page->present = 1;
    page->rw = 1;
    page->user = 1;
    page->accessed = 0;
    page->dirty = 0;
    page->shared = 1;
    paging_invlpg(vaddr & ~0xFFFU);
    return 1;
}

int paging_zero_user(address_space_t *space, uint32_t vaddr, uint32_t size) {
    if (size > UINT32_MAX - vaddr) return 0;

    while (size > 0) {
        uint8_t *dest = user_pointer(space, vaddr);
        uint32_t chunk;

        if (!dest) return 0;

        chunk = PMM_BLOCK_SIZE - (vaddr & 0xFFFU);
        if (chunk > size) chunk = size;
        memset(dest, 0, chunk);
        vaddr += chunk;
        size -= chunk;
    }
    return 1;
}

int paging_copy_to_user(address_space_t *space, uint32_t vaddr,
                        const uint8_t *src, uint32_t size) {
    if ((!src && size > 0) || size > UINT32_MAX - vaddr) return 0;

    while (size > 0) {
        uint8_t *dest = user_pointer(space, vaddr);
        uint32_t chunk;

        if (!dest) return 0;

        chunk = PMM_BLOCK_SIZE - (vaddr & 0xFFFU);
        if (chunk > size) chunk = size;
        memcpy(dest, src, chunk);
        src += chunk;
        vaddr += chunk;
        size -= chunk;
    }
    return 1;
}

int paging_user_range_mapped(uint32_t vaddr, uint32_t size) {
    uint32_t first_idx;
    uint32_t last_idx;

    if (size == 0) return 1;
    if (!active_space || size > UINT32_MAX - vaddr) return 0;

    first_idx = vaddr >> 12;
    last_idx = (vaddr + size - 1) >> 12;
    for (uint32_t idx = first_idx; idx <= last_idx; idx++) {
        page_entry_t *page = user_pte(active_space, idx << 12);
        if (!page || !page->present || !page->user) return 0;
    }
    return 1;
}

uint32_t paging_get_user_page_count(void) {
    uint32_t count = 0;

    if (!active_space) return 0;

    page_table_t *tables[2] = { active_space->first_table, active_space->ext_table };
    for (int t = 0; t < 2; t++) {
        if (!tables[t]) continue;
        for (int i = 0; i < 1024; i++) {
            page_entry_t *page = &tables[t]->pages[i];
            if (page->present && page->user) count++;
        }
    }
    return count;
}

uint32_t paging_get_user_space_count(void) {
    return user_space_count;
}
