#include "user_syscall.h"

/*
 * mmaptest - exercise the mmap-region page allocator end to end.
 *
 * Mapping: map 2MB via sys_mmap (which lives at virtual 32MB, above the kernel
 * identity map), write a distinct byte into every one of its 512 pages, and
 * read them all back. The base address (33554432 = 0x2000000) alone shows the
 * allocation is far past the old 4MB ceiling; a correct read-back of all 512
 * demand-paged pages shows the extended page table and fault path work.
 *
 * munmap: freeing the whole chunk must let the very next sys_mmap hand the
 * same base out again with zero-filled pages (the frames really went back to
 * the allocator, and no old data leaks through). Freeing a chunk between two
 * live ones must leave the neighbours intact and make the hole reusable by
 * first fit, and a double free must fail. Finally, touching a freed page must
 * be fatal -- a forked child tries it and the parent checks that it was killed
 * by the page-fault handler instead of reaching its exit().
 */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

#define NPAGES 512   /* 2 MB */

int main(void) {
    unsigned char *p = (unsigned char *)sys_mmap(NPAGES);
    if (!p) {
        write_str("[mmap] failed\n");
        return 1;
    }

    write_str("[mmap] base=");
    write_int((int)(unsigned long)p);
    write_str("\n");

    for (int i = 0; i < NPAGES; i++) p[i * 4096] = (unsigned char)(i & 0xFF);
    p[NPAGES * 4096 - 1] = 0x5A;   /* touch the very top of the 2MB region */

    int ok = 1;
    for (int i = 0; i < NPAGES; i++)
        if (p[i * 4096] != (unsigned char)(i & 0xFF)) ok = 0;
    if (p[NPAGES * 4096 - 1] != 0x5A) ok = 0;

    write_str(ok ? "[mmap ok]\n" : "[mmap FAIL]\n");

    /* munmap the whole chunk: the next same-size mmap must reuse its address,
     * and the remapped pages must read back zero, not the old pattern. */
    ok = sys_munmap(p, NPAGES) == 0;
    unsigned char *q = (unsigned char *)sys_mmap(NPAGES);
    if (q != p) ok = 0;
    if (q[4096] != 0 || q[NPAGES * 4096 - 1] != 0) ok = 0;
    write_str(ok ? "[munmap reuse ok]\n" : "[munmap reuse FAIL]\n");

    /* Free a chunk between two live ones: the neighbours must survive, first
     * fit must refill the hole (zeroed), and a double free must fail. */
    unsigned char *a = (unsigned char *)sys_mmap(4);
    unsigned char *b = (unsigned char *)sys_mmap(4);
    unsigned char *c = (unsigned char *)sys_mmap(4);
    ok = a && b && c;
    if (ok) {
        a[0] = 11; b[0] = 22; c[0] = 33;
        if (sys_munmap(b, 4) != 0) ok = 0;
        unsigned char *b2 = (unsigned char *)sys_mmap(4);
        if (b2 != b || b2[0] != 0) ok = 0;
        if (a[0] != 11 || c[0] != 33) ok = 0;
        if (sys_munmap(b2, 4) != 0) ok = 0;
        if (sys_munmap(b2, 4) == 0) ok = 0;   /* double free must fail */
        if (sys_munmap(a, 4) != 0 || sys_munmap(c, 4) != 0) ok = 0;
    }
    write_str(ok ? "[munmap hole ok]\n" : "[munmap hole FAIL]\n");

    /* Touching a freed page must be fatal. A child tries it so the parent
     * survives to check that the kernel killed it before its exit(42). The
     * pointer must be volatile: a plain store just before exit() is dead to
     * the optimizer and would be deleted, and then nothing ever faults. */
    sys_munmap(q, NPAGES);
    volatile unsigned char *freed = q;
    int pid = sys_fork();
    if (pid == 0) {
        freed[0] = 1;     /* freed page: the fault handler kills us here */
        sys_exit(42);     /* never reached */
    }
    int status = 0;
    sys_waitpid(pid, &status, 0);
    write_str(status != 42 ? "[munmap fault ok]\n" : "[munmap fault FAIL]\n");
    return 0;
}
