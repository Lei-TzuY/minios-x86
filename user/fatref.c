#include "user_syscall.h"

/*
 * fatref - regression test for FAT16 open-file reference counting.
 *
 * fat16_vfs_unlink() used to free a file's cluster chain with no regard for
 * open descriptors (unlike RAMFS and DiskFS, which both refuse). Those
 * clusters went straight back to the free pool, so a later write could hand
 * them to a different file while an existing descriptor still read through
 * them -- returning another file's contents.
 *
 * Here /fat/hello.txt is held open while unlink is attempted: it must be
 * REFUSED, and the descriptor must still read the real contents. The file is
 * never actually removed, so the rest of the test suite still sees it.
 */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

#define HELLO      "Hello from FAT16!\n"
#define HELLO_LEN  18

int main(void) {
    char buf[32];
    struct ustat st;
    int fd, n, ok;

    fd = sys_open("/fat/hello.txt");
    if (fd < 0) { write_str("[fatref] open failed\n"); return 1; }

    if (sys_unlink("/fat/hello.txt") != 0)
        write_str("[fatref inuse unlink refused]\n");
    else
        write_str("[fatref inuse unlink WRONGLY allowed]\n");

    /* The chain must still be intact and readable through the descriptor. */
    n = sys_read_file(fd, buf, HELLO_LEN);
    ok = (n == HELLO_LEN);
    for (int i = 0; ok && i < HELLO_LEN; i++)
        if (buf[i] != HELLO[i]) ok = 0;
    sys_close(fd);
    write_str(ok ? "[fatref content ok]\n" : "[fatref content FAIL]\n");

    /* And the file must still exist afterwards (the unlink really was a no-op). */
    if (sys_stat("/fat/hello.txt", &st) == 0 && st.size == HELLO_LEN)
        write_str("[fatref file intact]\n");
    else
        write_str("[fatref file intact FAIL]\n");

    write_str("[fatref done]\n");
    return 0;
}
