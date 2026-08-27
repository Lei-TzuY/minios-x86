#include "user_syscall.h"

#define PAGE_SIZE 4096
#define HEAP_PAGES 16
#define MMAP_PAGES 32

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

int main(void) {
    unsigned char *heap = (unsigned char *)sys_sbrk(HEAP_PAGES * PAGE_SIZE);
    unsigned char *mapped = (unsigned char *)sys_mmap(MMAP_PAGES);
    int pipefd[2];
    int fd;
    volatile int *kernel_address = (volatile int *)0x200000;

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

    write_str("[fault resources armed]\n");

    /* This supervisor-only identity-mapped address must terminate just this
     * ring-3 process.  Reaching the return would mean the isolation failed. */
    return *kernel_address;
}
