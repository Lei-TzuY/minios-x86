#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>
#include <stddef.h>

typedef struct heap_block {
    size_t size;
    int is_free;
    struct heap_block *next;
} heap_block_t;

void heap_init(void);
void *kmalloc(size_t size);
void kfree(void *ptr);
size_t heap_get_page_count(void);
size_t heap_get_free_bytes(void);
/* First block of the free list, for tests that need to walk it and check
 * structural invariants (no overlap, growth only by adjacency). Not used
 * by the kernel itself. */
heap_block_t *heap_first_block(void);

#endif
