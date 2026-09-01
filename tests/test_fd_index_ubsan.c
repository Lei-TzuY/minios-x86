#include <stdint.h>

/* Reuse the real descriptor-table harness and all of its counting stubs in the
 * same translation unit, but replace its normal main with this focused entry
 * point.  This keeps the regression coupled to the real syscall.c while
 * avoiding a second copy of the large hosted stub surface. */
#define main fdtable_full_suite_main
#include "test_fdtable.c"
#undef main

int main(void) {
    static const int32_t invalid_fds[] = { INT32_MIN, INT32_MAX };
    process_t *process;

    TEST("extreme user descriptors never overflow slot conversion");
    reset_world();
    process = take_slot(0, 10);
    g_current = process;

    CHECK_EQ(open_user_file(&g_nodes[0]), FIRST_USER_FD);
    CHECK_EQ(g_node_refs[0], 1);

    for (size_t i = 0; i < sizeof(invalid_fds) / sizeof(invalid_fds[0]); i++) {
        int32_t fd = invalid_fds[i];

        /* Invalid descriptors must be rejected before any user-pointer access.
         * On the old implementation INT32_MIN reached `fd - FIRST_USER_FD`
         * first, which is signed overflow and is trapped by this UBSan build. */
        CHECK_EQ(sys_read_file(fd, NULL, 0), -1);
        CHECK_EQ(sys_write_file(fd, NULL, 0), -1);
        CHECK_EQ(sys_seek(fd, 0, SYS_SEEK_SET), -1);
        CHECK_EQ(sys_fstat(fd, NULL), -1);
        CHECK_EQ(sys_dup(fd), -1);
        CHECK_EQ(sys_dup2(fd, FIRST_USER_FD), -1);
        CHECK_EQ(sys_close(fd), -1);
    }

    /* Rejections must not touch the live descriptor or its ownership. */
    CHECK_EQ(open_files[0][0].kind, OF_FILE);
    CHECK(open_files[0][0].node == &g_nodes[0]);
    CHECK_EQ(g_node_refs[0], 1);
    expect_no_underflow();

    CHECK_EQ(sys_close(FIRST_USER_FD), 0);
    CHECK_EQ(g_node_refs[0], 0);
    expect_no_underflow();

    TEST_REPORT("fd-index-ubsan");
}
