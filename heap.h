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

#endif
