#ifndef UMALLOC_H
#define UMALLOC_H

/*
 * Minimal user-space dynamic allocator for miniOS programs.
 *
 * A classic K&R free-list allocator backed by the SYS_SBRK system call.
 * malloc() carves blocks from a circular free list; free() coalesces
 * adjacent blocks back into it. The heap only ever grows (via sbrk); freed
 * memory is reused but not returned to the kernel until the process exits.
 *
 * Header-only: each translation unit that includes this gets its own private
 * allocator state, which is fine because every miniOS program is its own ELF.
 */

#include "user_syscall.h"

typedef long Align;  /* force worst-case alignment of allocated blocks */

union umalloc_header {
    struct {
        union umalloc_header *next;  /* next free block */
        unsigned size;              /* size of this block in header units */
    } s;
    Align align;                    /* never referenced; forces alignment */
};

typedef union umalloc_header Header;

static Header umalloc_base;           /* empty list to start */
static Header *umalloc_freep = 0;     /* start of the free list */

#define UMALLOC_NALLOC 1024u          /* min units requested from sbrk */

static inline void free(void *ap);

/* Ask the kernel for more heap and insert it as a free block. */
static inline Header *umalloc_morecore(unsigned nu) {
    char *cp;
    Header *up;

    if (nu < UMALLOC_NALLOC) nu = UMALLOC_NALLOC;

    /* sbrk takes a signed increment. Reject sizes whose byte count would not
     * fit, rather than letting the cast wrap negative -- that would be read as
     * a request to SHRINK the heap, and sbrk would happily return a "valid"
     * pointer into memory the program does not own. */
    if (nu > (unsigned)0x7FFFFFFF / sizeof(Header)) return 0;

    cp = (char *)sys_sbrk((int)(nu * sizeof(Header)));
    if (cp == (char *)-1) return 0;   /* out of memory */

    up = (Header *)cp;
    up->s.size = nu;
    free((void *)(up + 1));
    return umalloc_freep;
}

static inline void *malloc(unsigned nbytes) {
    Header *p, *prevp;
    unsigned nunits;

    if (nbytes == 0) return 0;
    nunits = (nbytes + sizeof(Header) - 1) / sizeof(Header) + 1;

    if ((prevp = umalloc_freep) == 0) {  /* no free list yet: initialise */
        umalloc_base.s.next = umalloc_freep = prevp = &umalloc_base;
        umalloc_base.s.size = 0;
    }

    for (p = prevp->s.next; ; prevp = p, p = p->s.next) {
        if (p->s.size >= nunits) {       /* big enough */
            if (p->s.size == nunits) {   /* exact fit: unlink */
                prevp->s.next = p->s.next;
            } else {                     /* split off the tail */
                p->s.size -= nunits;
                p += p->s.size;
                p->s.size = nunits;
            }
            umalloc_freep = prevp;
            return (void *)(p + 1);
        }
        if (p == umalloc_freep) {        /* wrapped the list: grow */
            if ((p = umalloc_morecore(nunits)) == 0) return 0;
        }
    }
}

static inline void free(void *ap) {
    Header *bp, *p;

    if (!ap) return;
    bp = (Header *)ap - 1;               /* point to block header */

    for (p = umalloc_freep; !(bp > p && bp < p->s.next); p = p->s.next) {
        if (p >= p->s.next && (bp > p || bp < p->s.next))
            break;                       /* freed block at start/end of arena */
    }

    if (bp + bp->s.size == p->s.next) {  /* coalesce with upper neighbour */
        bp->s.size += p->s.next->s.size;
        bp->s.next = p->s.next->s.next;
    } else {
        bp->s.next = p->s.next;
    }

    if (p + p->s.size == bp) {           /* coalesce with lower neighbour */
        p->s.size += bp->s.size;
        p->s.next = bp->s.next;
    } else {
        p->s.next = bp;
    }

    umalloc_freep = p;
}

static inline void *calloc(unsigned count, unsigned size) {
    unsigned total = count * size;
    char *p;

    if (count != 0 && total / count != size) return 0;  /* overflow */
    p = (char *)malloc(total);
    if (p) {
        for (unsigned i = 0; i < total; i++) p[i] = 0;
    }
    return p;
}

/* Usable payload size of an allocated block, in bytes. */
static inline unsigned umalloc_usable(void *ap) {
    Header *bp = (Header *)ap - 1;
    return (bp->s.size - 1) * sizeof(Header);
}

static inline void *realloc(void *ap, unsigned nbytes) {
    void *np;
    unsigned old, i;

    if (!ap) return malloc(nbytes);
    if (nbytes == 0) { free(ap); return 0; }

    old = umalloc_usable(ap);
    if (old >= nbytes) return ap;        /* current block already fits */

    np = malloc(nbytes);
    if (!np) return 0;
    for (i = 0; i < old; i++) ((char *)np)[i] = ((char *)ap)[i];
    free(ap);
    return np;
}

#endif
