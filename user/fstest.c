#include "user_syscall.h"

static void write_str(const char *s) {
    int len = 0;
    while (s[len]) len++;
    sys_write(s, len);
}

static int strings_equal(const char *first, const char *second, int length) {
    for (int i = 0; i < length; i++) {
        if (first[i] != second[i]) return 0;
    }
    return 1;
}

int main(void) {
    static const char payload[] = "abcdef";
    static const char patch[] = "XY";
    static const char expected[] = "abXYef";
    static const char tail[] = "!";
    static const char disk_seed[] = "MINIOS DISKFS SEED\n";
    static const char disk_payload[] = "user vfs disk write works\n";
    char buffer[128];
    int fd;
    int count;

    fd = sys_create("notes.txt");
    if (fd < 0 || sys_create("notes.txt") >= 0 ||
        sys_unlink("notes.txt") >= 0 ||
        sys_write_file(fd, payload, sizeof(payload) - 1) != sizeof(payload) - 1 ||
        sys_seek(fd, 2, SEEK_SET) != 2 ||
        sys_write_file(fd, patch, sizeof(patch) - 1) != sizeof(patch) - 1 ||
        sys_seek(fd, 2, SEEK_CUR) != 6 ||
        sys_seek(fd, 10, SEEK_SET) != 10 ||
        sys_write_file(fd, tail, sizeof(tail) - 1) != sizeof(tail) - 1 ||
        sys_seek(fd, -1, SEEK_END) != 10 ||
        sys_read_file(fd, buffer, 1) != 1 || buffer[0] != '!' ||
        sys_seek(fd, -1, SEEK_SET) >= 0 ||
        sys_seek(fd, 0, 99) >= 0 ||
        sys_close(fd) != 0) {
        write_str("[ramfs create/write test failed]\n");
        return 1;
    }

    fd = sys_open("notes.txt");
    if (fd < 0) {
        write_str("[ramfs read/unlink test failed]\n");
        return 1;
    }
    count = sys_read_file(fd, buffer, sizeof(buffer));
    if (count != 11 ||
        !strings_equal(buffer, expected, sizeof(expected) - 1) ||
        buffer[6] != 0 || buffer[7] != 0 || buffer[8] != 0 ||
        buffer[9] != 0 || buffer[10] != '!' ||
        sys_close(fd) != 0 ||
        sys_unlink("notes.txt") != 0 ||
        sys_open("notes.txt") >= 0) {
        write_str("[ramfs read/unlink test failed]\n");
        return 1;
    }

    for (int i = 0; i < 20; i++) {
        fd = sys_create("reusable.tmp");
        if (fd < 0 || sys_close(fd) != 0 ||
            sys_unlink("reusable.tmp") != 0) {
            write_str("[ramfs reuse test failed]\n");
            return 1;
        }
    }

    if (sys_create("bad//name") >= 0) {
        write_str("[ramfs name test failed]\n");
        return 1;
    }

    if (sys_mkdir("docs") != 0 ||
        sys_mkdir("docs") >= 0 ||
        sys_mkdir("docs/archive") != 0 ||
        sys_mkdir("missing/child") >= 0 ||
        sys_mkdir("docs//bad") >= 0) {
        write_str("[ramfs mkdir test failed]\n");
        return 1;
    }

    fd = sys_create("docs/archive/note.txt");
    if (fd < 0 ||
        sys_write_file(fd, payload, sizeof(payload) - 1) != sizeof(payload) - 1 ||
        sys_close(fd) != 0) {
        write_str("[ramfs nested write test failed]\n");
        return 1;
    }

    fd = sys_open("/docs/archive/note.txt");
    if (fd < 0) {
        write_str("[ramfs nested read test failed]\n");
        return 1;
    }
    count = sys_read_file(fd, buffer, sizeof(buffer));
    if (count != sizeof(payload) - 1 ||
        !strings_equal(buffer, payload, count) ||
        sys_close(fd) != 0 ||
        sys_readdir("docs", 0, buffer) != 1 ||
        !strings_equal(buffer, "archive", sizeof("archive")) ||
        sys_readdir("docs", 1, buffer) != 0 ||
        sys_readdir("docs/archive", 0, buffer) != 1 ||
        !strings_equal(buffer, "note.txt", sizeof("note.txt")) ||
        sys_readdir("docs/archive/note.txt", 0, buffer) >= 0 ||
        sys_rmdir("docs") >= 0 ||
        sys_unlink("docs/archive/note.txt") != 0 ||
        sys_rmdir("docs/archive") != 0 ||
        sys_rmdir("docs") != 0 ||
        sys_open("docs/archive/note.txt") >= 0 ||
        sys_rmdir("/") >= 0) {
        write_str("[ramfs nested cleanup test failed]\n");
        return 1;
    }

    fd = sys_open("readme.txt");
    if (fd < 0 || sys_write_file(fd, payload, sizeof(payload) - 1) >= 0 ||
        sys_close(fd) != 0 || sys_unlink("readme.txt") >= 0) {
        write_str("[ramfs readonly test failed]\n");
        return 1;
    }

    if (sys_readdir("/disk", 0, buffer) != 1 ||
        !strings_equal(buffer, "seed.txt", sizeof("seed.txt")) ||
        sys_readdir("/disk", 1, buffer) != 0) {
        write_str("[diskfs vfs readdir test failed]\n");
        return 1;
    }

    fd = sys_open("/disk/seed.txt");
    if (fd < 0) {
        write_str("[diskfs vfs seed read test failed]\n");
        return 1;
    }
    count = sys_read_file(fd, buffer, sizeof(buffer));
    if (count != sizeof(disk_seed) - 1 ||
        !strings_equal(buffer, disk_seed, count) ||
        sys_close(fd) != 0) {
        write_str("[diskfs vfs seed read test failed]\n");
        return 1;
    }

    fd = sys_create("/disk/user.txt");
    if (fd < 0 ||
        sys_create("/disk/user.txt") >= 0 ||
        sys_write_file(fd, disk_payload, sizeof(disk_payload) - 1) !=
            sizeof(disk_payload) - 1 ||
        sys_unlink("/disk/user.txt") >= 0 ||
        sys_close(fd) != 0) {
        write_str("[diskfs vfs write test failed]\n");
        return 1;
    }

    fd = sys_open("/disk/user.txt");
    if (fd < 0) {
        write_str("[diskfs vfs read/unlink test failed]\n");
        return 1;
    }
    count = sys_read_file(fd, buffer, sizeof(buffer));
    if (count != sizeof(disk_payload) - 1 ||
        !strings_equal(buffer, disk_payload, count) ||
        sys_close(fd) != 0 ||
        sys_unlink("/disk/user.txt") != 0 ||
        sys_open("/disk/user.txt") >= 0) {
        write_str("[diskfs vfs read/unlink test failed]\n");
        return 1;
    }

    if (sys_mkdir("/disk/docs") != 0 ||
        sys_mkdir("/disk/docs") >= 0 ||
        sys_mkdir("/disk/docs/archive") != 0 ||
        sys_mkdir("/disk/missing/child") >= 0) {
        write_str("[diskfs vfs mkdir test failed]\n");
        return 1;
    }

    fd = sys_create("/disk/docs/archive/note.txt");
    if (fd < 0 ||
        sys_write_file(fd, disk_payload, sizeof(disk_payload) - 1) !=
            sizeof(disk_payload) - 1 ||
        sys_close(fd) != 0) {
        write_str("[diskfs vfs nested write test failed]\n");
        return 1;
    }

    fd = sys_open("/disk/docs/archive/note.txt");
    if (fd < 0) {
        write_str("[diskfs vfs nested read test failed]\n");
        return 1;
    }
    count = sys_read_file(fd, buffer, sizeof(buffer));
    if (count != sizeof(disk_payload) - 1 ||
        !strings_equal(buffer, disk_payload, count) ||
        sys_close(fd) != 0 ||
        sys_readdir("/disk/docs", 0, buffer) != 1 ||
        !strings_equal(buffer, "archive", sizeof("archive")) ||
        sys_readdir("/disk/docs", 1, buffer) != 0 ||
        sys_readdir("/disk/docs/archive", 0, buffer) != 1 ||
        !strings_equal(buffer, "note.txt", sizeof("note.txt")) ||
        sys_rmdir("/disk/docs") >= 0 ||
        sys_unlink("/disk/docs/archive/note.txt") != 0 ||
        sys_rmdir("/disk/docs/archive") != 0 ||
        sys_rmdir("/disk/docs") != 0 ||
        sys_open("/disk/docs/archive/note.txt") >= 0 ||
        sys_rmdir("/disk") >= 0) {
        write_str("[diskfs vfs nested cleanup test failed]\n");
        return 1;
    }

    write_str("[ramfs readdir test passed]\n");
    write_str("[ramfs path test passed]\n");
    write_str("[ramfs seek test passed]\n");
    write_str("[ramfs write test passed]\n");
    write_str("[diskfs vfs test passed]\n");
    write_str("[diskfs nested directory test passed]\n");
    return 0;
}
