#include "user_syscall.h"

/* stat/fstat demo: query file metadata by path and by open descriptor. */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

int main(void) {
    struct ustat st;
    int fd;

    if (sys_stat("/fat/hello.txt", &st) == 0) {
        write_str("[file size=");
        write_int((int)st.size);
        write_str(" type=");
        write_int((int)st.type);
        write_str("]\n");
    }

    if (sys_stat("/fat", &st) == 0 && S_ISDIR(st.type))
        write_str("[fat is a directory]\n");

    if (sys_stat("/nope.txt", &st) != 0)
        write_str("[stat missing rejected]\n");

    fd = sys_open("/fat/hello.txt");
    if (fd >= 0 && sys_fstat(fd, &st) == 0) {
        write_str("[fstat size=");
        write_int((int)st.size);
        write_str("]\n");
        sys_close(fd);
    }

    return 0;
}
