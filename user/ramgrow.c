#include "user_syscall.h"

/*
 * ramgrow - exercises the amortized RAMFS file-growth path. It appends a
 * repeating pattern to a file in many small writes (previously each growing
 * write reallocated and copied the whole file, i.e. O(n^2) to build up; now
 * the buffer grows geometrically), then reads it all back and verifies every
 * byte. The file is removed before exit so it does not perturb the RAMFS node
 * count that other tests assert on.
 */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

#define CHUNK  16
#define ROUNDS 128                 /* 128 * 16 = 2048 bytes total */
#define TOTAL  (CHUNK * ROUNDS)

/* Byte value at absolute file position p: a repeating 'a'..'p' pattern. */
static char pattern_byte(int p) { return (char)('a' + (p % CHUNK)); }

int main(void) {
    char chunk[CHUNK];
    for (int j = 0; j < CHUNK; j++) chunk[j] = pattern_byte(j);

    int fd = sys_create("ramgrow.tmp");
    if (fd < 0) { write_str("[ramgrow] create failed\n"); return 1; }

    for (int i = 0; i < ROUNDS; i++) {
        if (sys_write_file(fd, chunk, CHUNK) != CHUNK) {
            write_str("[ramgrow] write failed\n");
            sys_close(fd);
            sys_unlink("ramgrow.tmp");
            return 1;
        }
    }
    sys_close(fd);

    fd = sys_open("ramgrow.tmp");
    if (fd < 0) { write_str("[ramgrow] open failed\n"); return 1; }

    int total = 0, ok = 1, n;
    char buf[40];
    while ((n = sys_read_file(fd, buf, sizeof(buf))) > 0) {
        for (int j = 0; j < n; j++)
            if (buf[j] != pattern_byte(total + j)) ok = 0;
        total += n;
    }
    sys_close(fd);
    sys_unlink("ramgrow.tmp");

    write_str("[ramgrow bytes=");
    write_int(total);
    write_str("]\n");
    write_str((ok && total == TOTAL) ? "[ramgrow ok]\n" : "[ramgrow FAIL]\n");
    return 0;
}
