#include "user_syscall.h"

/*
 * fatgrow - regression test for F23: a FAT16 write that stores nothing must
 * not grow the file.
 *
 * fat16_vfs_write() records offset + written as the new end of file. That is
 * the true end only when something was actually written there. Start a write
 * beyond the last cluster the chain can be extended to -- which happens as
 * soon as the volume is full -- and not a single byte is stored, yet the file
 * used to grow to the seek offset anyway. It then advertised a length its own
 * cluster chain could not back, and a read served up the tail of its last real
 * cluster as if it were file data.
 *
 * Everything here is an ordinary ring-3 operation: the /fat volume is 32 KB,
 * and sys_seek accepts any offset up to 0x7FFFFFFF. Same seek-past-the-end
 * shape as F22, against a different filesystem.
 *
 * The unit test (tests/test_fat16.c, "write that stores nothing") pins the
 * driver logic; this one proves the whole syscall path reaches it from user
 * space. It restores the volume before exiting -- both files are removed, so
 * their clusters go back to the free pool and the later FAT assertions in the
 * suite still see the image they expect.
 */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

#define CHUNK 512

static char filler[CHUNK];

int main(void) {
    struct ustat st;
    char probe[16];
    int small, fill, n, i;
    int total = 0;

    /* A file that owns exactly one cluster. */
    small = sys_create("/fat/fg.txt");
    if (small < 0) { write_str("[fatgrow] create failed\n"); return 1; }
    if (sys_write_file(small, "data", 4) != 4) {
        write_str("[fatgrow] setup write failed\n");
        return 1;
    }

    /* Consume every remaining cluster on the volume. */
    for (i = 0; i < CHUNK; i++) filler[i] = 'F';
    fill = sys_create("/fat/fgfill.txt");
    if (fill < 0) { write_str("[fatgrow] fill create failed\n"); return 1; }
    for (i = 0; i < 200; i++) {
        n = sys_write_file(fill, filler, CHUNK);
        if (n <= 0) break;
        total += n;
        if (n < CHUNK) break;              /* the last partial cluster */
    }
    if (total == 0) { write_str("[fatgrow] fill wrote nothing\n"); return 1; }

    write_str("[fatgrow armed]\n");

    /* Well past the single cluster fg.txt owns, with nothing left to allocate.
     * The write must fail -- and must leave the file exactly as it was. */
    if (sys_seek(small, 4096, SEEK_SET) != 4096) {
        write_str("[fatgrow] seek rejected\n");
        return 1;
    }
    n = sys_write_file(small, "zzzz", 4);
    write_str(n <= 0 ? "[fatgrow write refused]\n"
                     : "[fatgrow write WRONGLY stored]\n");

    /* The length is the assertion that matters: before the fix the file grew
     * to 4096 despite holding four bytes. */
    if (sys_stat("/fat/fg.txt", &st) == 0 && st.size == 4)
        write_str("[fatgrow size intact]\n");
    else
        write_str("[fatgrow size FAIL]\n");

    /* And it still reads back as those four bytes, not a cluster's worth of
     * whatever happened to follow them. */
    for (i = 0; i < (int)sizeof(probe); i++) probe[i] = '?';
    if (sys_seek(small, 0, SEEK_SET) != 0) {
        write_str("[fatgrow] rewind failed\n");
        return 1;
    }
    n = sys_read_file(small, probe, sizeof(probe));
    if (n == 4 && probe[0] == 'd' && probe[1] == 'a' &&
        probe[2] == 't' && probe[3] == 'a')
        write_str("[fatgrow read intact]\n");
    else
        write_str("[fatgrow read FAIL]\n");

    /* Put the volume back the way it was: unlink frees both cluster chains, so
     * the FAT assertions later in the suite still have room to write. */
    sys_close(small);
    sys_close(fill);
    if (sys_unlink("/fat/fg.txt") != 0) write_str("[fatgrow] cleanup fg failed\n");
    if (sys_unlink("/fat/fgfill.txt") != 0)
        write_str("[fatgrow] cleanup fill failed\n");

    write_str("[fatgrow done]\n");
    return 0;
}
