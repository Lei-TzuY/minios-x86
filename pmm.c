#include "pmm.h"

/* The paging layer identity-maps the first 32MB for kernel allocations.
 * 32MB / 4KB = 8192 blocks. 8192 bits = 1024 bytes (256 uint32_t). */
#define PMM_MAX_BLOCKS (PMM_MAX_MEMORY / PMM_BLOCK_SIZE)

static uint32_t memory_map[PMM_MAX_BLOCKS / 32];
static uint32_t max_blocks = 0;
static uint32_t used_blocks = 0;

static inline int mmap_test(uint32_t bit) {
    return memory_map[bit / 32] & (1U << (bit % 32));
}

static inline void mmap_set(uint32_t bit) {
    memory_map[bit / 32] |= (1U << (bit % 32));
}

static inline void mmap_unset(uint32_t bit) {
    memory_map[bit / 32] &= ~(1U << (bit % 32));
}

static int mmap_first_free_run(uint32_t count) {
    uint32_t run_start = 0;
    uint32_t run_length = 0;

    if (count == 0 || count > max_blocks) return -1;

    for (uint32_t frame = 1; frame < max_blocks; frame++) {
        if (!mmap_test(frame)) {
            if (run_length == 0) run_start = frame;
            if (++run_length == count) return (int)run_start;
        } else {
            run_length = 0;
        }
    }
    return -1;
}

void pmm_init(uint32_t mem_size_kb) {
    max_blocks = mem_size_kb / 4; // mem_size is in KB, block is 4KB
    if (max_blocks > PMM_MAX_BLOCKS) max_blocks = PMM_MAX_BLOCKS;
    used_blocks = max_blocks; /* By default, all memory is in use/reserved */
    /* Set all bits to 1 */
    for (uint32_t i = 0; i < PMM_MAX_BLOCKS / 32; i++) {
        memory_map[i] = 0xFFFFFFFF;
    }
}

void pmm_init_region(uint32_t base, uint32_t size) {
    uint64_t region_end = (uint64_t)base + size;
    uint32_t start = base / PMM_BLOCK_SIZE;
    uint32_t end;

    if (base % PMM_BLOCK_SIZE != 0) start++;
    end = region_end / PMM_BLOCK_SIZE;
    if (end > max_blocks) end = max_blocks;
    for (uint32_t frame = start; frame < end; frame++) {
        if (mmap_test(frame)) {
            mmap_unset(frame);
            used_blocks--;
        }
    }
    /* Block 0 is never handed out, so a successful allocation can never be
     * mistaken for NULL. Re-reserving it must update the accounting as well:
     * when a region starts at 0 the loop above has just released frame 0 and
     * decremented used_blocks, so setting the bit alone would leave the
     * manager advertising one more free block than it can actually supply. */
    if (!mmap_test(0)) {
        mmap_set(0);
        used_blocks++;
    }
}

void pmm_deinit_region(uint32_t base, uint32_t size) {
    uint64_t region_end = (uint64_t)base + size;
    uint32_t start = base / PMM_BLOCK_SIZE;
    uint32_t end = (region_end + PMM_BLOCK_SIZE - 1) / PMM_BLOCK_SIZE;

    if (end > max_blocks) end = max_blocks;
    for (uint32_t frame = start; frame < end; frame++) {
        if (!mmap_test(frame)) {
            mmap_set(frame);
            used_blocks++;
        }
    }
}

void* pmm_alloc_block(void) {
    return pmm_alloc_blocks(1);
}

void* pmm_alloc_blocks(uint32_t count) {
    int first_frame;

    if (count == 0 || count > max_blocks - used_blocks) return NULL;

    first_frame = mmap_first_free_run(count);
    if (first_frame < 0) return NULL;

    for (uint32_t i = 0; i < count; i++) {
        mmap_set((uint32_t)first_frame + i);
    }
    used_blocks += count;
    return (void*)((uint32_t)first_frame * PMM_BLOCK_SIZE);
}

void pmm_free_block(void* p) {
    pmm_free_blocks(p, 1);
}

void pmm_free_blocks(void* p, uint32_t count) {
    uint32_t addr = (uint32_t)p;
    uint32_t first_frame;

    if (!p || count == 0 || addr % PMM_BLOCK_SIZE != 0) return;

    first_frame = addr / PMM_BLOCK_SIZE;
    if (first_frame == 0 || first_frame >= max_blocks ||
        count > max_blocks - first_frame) {
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        uint32_t frame = first_frame + i;
        if (mmap_test(frame)) {
            mmap_unset(frame);
            used_blocks--;
        }
    }
}

uint32_t pmm_get_total_blocks(void) {
    return max_blocks;
}

uint32_t pmm_get_used_blocks(void) {
    return used_blocks;
}

uint32_t pmm_get_free_blocks(void) {
    return max_blocks - used_blocks;
}
