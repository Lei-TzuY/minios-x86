#include "heap.h"
#include "pmm.h"
#include "utils.h"

#define HEAP_ALIGNMENT 4

static heap_block_t *heap_head = NULL;
static size_t heap_pages = 0;

static int blocks_are_adjacent(heap_block_t *first, heap_block_t *second) {
    return (uint8_t *)first + sizeof(heap_block_t) + first->size ==
           (uint8_t *)second;
}

static void merge_free_blocks(void) {
    heap_block_t *current = heap_head;

    while (current && current->next) {
        if (current->is_free && current->next->is_free &&
            blocks_are_adjacent(current, current->next)) {
            current->size += sizeof(heap_block_t) + current->next->size;
            current->next = current->next->next;
        } else {
            current = current->next;
        }
    }
}

static void insert_block(heap_block_t *block) {
    heap_block_t *current;

    if (!heap_head || (uintptr_t)block < (uintptr_t)heap_head) {
        block->next = heap_head;
        heap_head = block;
        return;
    }

    current = heap_head;
    while (current->next && (uintptr_t)current->next < (uintptr_t)block) {
        current = current->next;
    }
    block->next = current->next;
    current->next = block;
}

static int heap_grow(size_t min_payload) {
    size_t total_size;
    uint32_t page_count;
    heap_block_t *block;

    if (min_payload > (size_t)-1 - sizeof(heap_block_t) -
                      (PMM_BLOCK_SIZE - 1)) {
        return 0;
    }

    total_size = sizeof(heap_block_t) + min_payload;
    page_count = (uint32_t)((total_size + PMM_BLOCK_SIZE - 1) /
                            PMM_BLOCK_SIZE);
    block = (heap_block_t *)pmm_alloc_blocks(page_count);
    if (!block) return 0;

    block->size = page_count * PMM_BLOCK_SIZE - sizeof(heap_block_t);
    block->is_free = 1;
    insert_block(block);
    heap_pages += page_count;
    merge_free_blocks();
    return 1;
}

void heap_init(void) {
    if (!heap_head) heap_grow(1);
}

void *kmalloc(size_t size) {
    heap_block_t *current;
    heap_block_t *best_fit;

    if (size == 0 || size > (size_t)-1 - (HEAP_ALIGNMENT - 1)) return NULL;

    size = (size + HEAP_ALIGNMENT - 1) & ~(HEAP_ALIGNMENT - 1);
    if (!heap_head) heap_init();

    current = heap_head;
    best_fit = NULL;

    /* First-fit search */
    while (current) {
        if (current->is_free && current->size >= size) {
            best_fit = current;
            break;
        }
        current = current->next;
    }

    if (!best_fit) {
        if (!heap_grow(size)) return NULL;

        current = heap_head;
        while (current) {
            if (current->is_free && current->size >= size) {
                best_fit = current;
                break;
            }
            current = current->next;
        }
        if (!best_fit) return NULL;
    }

    /* Split the block if it has enough extra space for another header + payload */
    if (best_fit->size > size + sizeof(heap_block_t) + 4) {
        heap_block_t *new_block = (heap_block_t *)((uint8_t *)best_fit + sizeof(heap_block_t) + size);
        new_block->size = best_fit->size - size - sizeof(heap_block_t);
        new_block->is_free = 1;
        new_block->next = best_fit->next;
        
        best_fit->size = size;
        best_fit->next = new_block;
    }

    best_fit->is_free = 0;
    return (void *)((uint8_t *)best_fit + sizeof(heap_block_t));
}

void kfree(void *ptr) {
    if (!ptr) return;

    /* Retrieve the block header */
    heap_block_t *block = (heap_block_t *)((uint8_t *)ptr - sizeof(heap_block_t));
    block->is_free = 1;

    /* Merge physically adjacent free blocks to prevent fragmentation. */
    merge_free_blocks();
}

heap_block_t *heap_first_block(void) {
    return heap_head;
}

size_t heap_get_page_count(void) {
    return heap_pages;
}

size_t heap_get_free_bytes(void) {
    size_t free_bytes = 0;
    heap_block_t *current = heap_head;

    while (current) {
        if (current->is_free) free_bytes += current->size;
        current = current->next;
    }
    return free_bytes;
}
