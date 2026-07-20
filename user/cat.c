#include "user_syscall.h"

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

int main(int argc, char **argv) {
    const char *filename = argc > 1 ? argv[1] : "readme.txt";
    char buffer[64];
    int fd = sys_open(filename);
    int count;

    if (fd < 0) {
        write_str("cat: cannot open: ");
        write_str(filename);
        sys_write("\n", 1);
        return 1;
    }

    while ((count = sys_read_file(fd, buffer, sizeof(buffer))) > 0) {
        sys_write(buffer, count);
    }

    if (count < 0) {
        write_str("cat: read error\n");
        sys_close(fd);
        return 1;
    }

    sys_close(fd);
    return 0;
}
