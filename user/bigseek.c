#include "user_syscall.h"

/*
 * bigseek - regression test for F22: a write at a huge offset must not hang the
 * kernel.
 *
 * ramfs_write sizes its buffer by doubling 64, 128, 256, ... until the capacity
 * covers offset+size. The overflow guard fired one step too late: at 0x80000000
 * the next doubling wrapped to 0, and 0 neither reaches the target nor trips the
 * guard, so the loop spun forever. sys_seek accepts an offset up to 0x7FFFFFFF,
 * so a two-byte write from ring 3 is enough to reach it -- and int 0x80 is an
 * interrupt gate, so the spin runs with interrupts disabled: no timer, no
 * keyboard, no scheduler. The whole machine stops, exactly like F2 and F14.
 *
 * The test simply performs the attack. Reaching the end at all is the result:
 * before the fix QEMU freezes here and the suite dies on its timeout, taking
 * every later assertion with it. The write itself is expected to FAIL (2GB
 * cannot be allocated) -- what matters is that it fails instead of spinning.
 */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

int main(void) {
    int fd;
    int n;

    /* sys_create already returns an OPEN descriptor -- opening it again would
     * take a second reference, and the unlink at the end would then be refused
     * (correctly: the file is still open), leaving the node behind and shifting
     * the suite's RAMFS node count. */
    fd = sys_create("bs.tmp");
    if (fd < 0) {
        write_str("[bigseek] create failed\n");
        return 1;
    }

    /* Anchor the file with a normal write so it has a real buffer first. */
    if (sys_write_file(fd, "hi", 2) != 2) {
        write_str("[bigseek] setup write failed\n");
        return 1;
    }

    write_str("[bigseek arming]\n");

    /* The largest offset sys_seek permits. */
    if (sys_seek(fd, 0x7FFFFFFF, SEEK_SET) != 0x7FFFFFFF) {
        write_str("[bigseek] seek rejected\n");
        return 1;
    }

    n = sys_write_file(fd, "ab", 2);      /* must return, not spin */
    write_str(n <= 0 ? "[bigseek refused]\n" : "[bigseek unexpectedly wrote]\n");

    /* Getting here proves the kernel is still alive and scheduling us. */
    write_str("[bigseek survived]\n");

    sys_close(fd);
    /* Assert the cleanup rather than assuming it: a leftover node would drift
     * the suite's RAMFS node count, and the reason (a reference still held)
     * would be far from obvious later. */
    if (sys_unlink("bs.tmp") != 0) write_str("[bigseek] cleanup failed\n");
    return 0;
}
