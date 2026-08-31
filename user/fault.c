#include "user_syscall.h"

#define PAGE_SIZE 4096
#define HEAP_PAGES 16
#define MMAP_PAGES 32

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "page";
    unsigned char *heap = (unsigned char *)sys_sbrk(HEAP_PAGES * PAGE_SIZE);
    unsigned char *mapped = (unsigned char *)sys_mmap(MMAP_PAGES);
    int pipefd[2];
    int fd;

    /* Leave every resource live on purpose.  The page-fault exit path, not
     * this program, must release the address space, file and both pipe ends. */
    fd = sys_open("/readme.txt");
    if (heap == (unsigned char *)-1 || !mapped || fd < 0 ||
        sys_pipe(pipefd) != 0) {
        write_str("[fault setup FAIL]\n");
        return 90;
    }

    for (int i = 0; i < HEAP_PAGES; i++)
        heap[i * PAGE_SIZE] = (unsigned char)(i * 7 + 3);
    for (int i = 0; i < MMAP_PAGES; i++)
        mapped[i * PAGE_SIZE] = (unsigned char)(i * 11 + 5);

    write_str("[fault resources armed mode=");
    write_str(mode);
    write_str("]\n");

    if (streq(mode, "page")) {
        volatile int *kernel_address = (volatile int *)0x200000;
        /* Supervisor-only identity mapping: #PF. */
        return *kernel_address;
    }
    if (streq(mode, "divide")) {
        unsigned dividend = 1;
        unsigned zero = 0;
        /* Runtime integer divide by zero: #DE. */
        __asm__ volatile("xorl %%edx, %%edx; divl %%ecx"
                         : "+a"(dividend)
                         : "c"(zero)
                         : "edx", "cc");
        return (int)dividend;
    }
    if (streq(mode, "invalid")) {
        __asm__ volatile("ud2"); /* #UD */
        return 92;
    }
    if (streq(mode, "privileged")) {
        __asm__ volatile("cli"); /* CPL3 privilege violation: #GP */
        return 93;
    }

    write_str("[fault mode FAIL]\n");
    return 94;
}
