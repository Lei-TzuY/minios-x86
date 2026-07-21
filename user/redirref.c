#include "user_syscall.h"

/*
 * redirref - regression test for a fixed kernel use-after-free.
 *
 * dup2() used to alias a file node onto stdin/stdout WITHOUT taking a VFS
 * reference, while every real descriptor holds one. The standard
 * "dup2(fd, N); close(fd);" idiom therefore left the stream pointing at a node
 * with refcount 0, so unlink() was free to kfree() it -- and the next read or
 * write went through a dangling pointer, calling a function pointer read out
 * of freed heap memory.
 *
 * Here stdin is aliased onto a file and the descriptor closed, leaving the
 * stream as the only holder. Unlinking the file must therefore be REFUSED
 * (the node is still in use), and reading stdin must still return the file's
 * contents. stdout is deliberately left alone so results stay visible.
 *
 * The file is intentionally still present when this exits (unlink was
 * refused); the test driver removes it afterwards, once process exit has
 * dropped the stream's reference.
 */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

int main(void) {
    int fd = sys_create("rr.tmp");
    if (fd < 0) { write_str("[redirref] create failed\n"); return 1; }
    sys_write_file(fd, "data", 4);
    sys_close(fd);

    fd = sys_open("rr.tmp");
    if (fd < 0) { write_str("[redirref] open failed\n"); return 1; }
    if (sys_dup2(fd, 0) < 0) {
        write_str("[redirref] dup2 failed\n");
        sys_close(fd);
        return 1;
    }
    sys_close(fd);            /* stdin is now the node's only reference */

    if (sys_unlink("rr.tmp") != 0)
        write_str("[redirref inuse unlink refused]\n");
    else
        write_str("[redirref inuse unlink WRONGLY allowed]\n");

    /* stdin really is the file, and the node is still alive to read from. */
    char buf[8];
    int n = sys_read(buf, 4);
    if (n == 4 && buf[0] == 'd' && buf[1] == 'a' &&
        buf[2] == 't' && buf[3] == 'a')
        write_str("[redirref stdin reads file]\n");
    else
        write_str("[redirref stdin read FAIL]\n");

    write_str("[redirref done]\n");
    return 0;
}
