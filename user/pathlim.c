#include "user_syscall.h"

/*
 * pathlim - regression test for a fixed VFS path-resolution bug.
 *
 * vfs_resolve_path() used to append components "best effort": when the
 * combined cwd + relative path exceeded FS_MAX_PATH it silently DROPPED the
 * trailing component and still reported success. The caller then received a
 * well-formed but *different* path -- an ancestor directory -- and would
 * operate on the wrong filesystem object.
 *
 * This test makes cwd long enough (a 125-character directory, so cwd is 126
 * characters) that resolving even the 1-character relative path "x" needs
 * 129 bytes and cannot fit. stat("x") must therefore FAIL. With the old
 * behaviour the "x" component was dropped, the path collapsed to the cwd, and
 * stat wrongly succeeded describing that directory.
 */

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

#define DIRNAME_LEN 125

int main(void) {
    char name[DIRNAME_LEN + 1];
    struct ustat st;
    int r;

    for (int i = 0; i < DIRNAME_LEN; i++) name[i] = 'd';
    name[DIRNAME_LEN] = '\0';

    if (sys_mkdir(name) != 0) { write_str("[pathlim] mkdir failed\n"); return 1; }
    if (sys_chdir(name) != 0) {
        write_str("[pathlim] chdir failed\n");
        sys_rmdir(name);
        return 1;
    }

    /* cwd is now "/" + 125 chars = 126 chars; "x" cannot be appended. */
    r = sys_stat("x", &st);
    if (r == -1) write_str("[pathlim overlong rejected]\n");
    else         write_str("[pathlim overlong WRONGLY accepted]\n");

    /* A short cwd must still resolve normally (guard against over-rejecting). */
    sys_chdir("/");
    r = sys_stat("readme.txt", &st);
    if (r == 0 && S_ISREG(st.type)) write_str("[pathlim normal path ok]\n");
    else                            write_str("[pathlim normal path FAIL]\n");

    sys_rmdir(name);
    write_str("[pathlim done]\n");
    return 0;
}
