/* Exercise user-controlled descriptor extremes against the real syscall.c.
 * Reuse test_fdtable.c's complete hosted stub world, but do not run its full
 * ownership suite here: this gate is deliberately focused on signed-overflow
 * safety and is compiled with UBSan in trap-on-error mode by its runner. */
#define main fdtable_regression_main
#include "test_fdtable.c"
#undef main

static void exercise_invalid_fd(int32_t fd) {
    CHECK_EQ(user_fd_index(fd), -1);
    CHECK_EQ(sys_read_file(fd, NULL, 0), -1);
    CHECK_EQ(sys_write_file(fd, NULL, 0), -1);
    CHECK_EQ(sys_seek(fd, 0, SYS_SEEK_SET), -1);
    CHECK_EQ(sys_fstat(fd, NULL), -1);
    CHECK_EQ(sys_dup(fd), -1);
    CHECK_EQ(sys_dup2(fd, 1), -1);
    CHECK_EQ(sys_close(fd), -1);
}

int main(void) {
    process_t *process;

    TEST("extreme invalid descriptors are rejected before slot arithmetic");
    reset_world();
    process = take_slot(0, 10);
    g_current = process;

    CHECK_EQ(open_user_file(&g_nodes[0]), FIRST_USER_FD);
    CHECK_EQ(user_fd_index(FIRST_USER_FD), 0);
    CHECK_EQ(user_fd_index(FIRST_USER_FD + MAX_OPEN_FILES - 1),
             MAX_OPEN_FILES - 1);

    exercise_invalid_fd(INT32_MIN);
    exercise_invalid_fd(INT32_MAX);

    /* Rejected descriptors must not disturb the one live ownership reference. */
    CHECK_EQ(g_node_refs[0], 1);
    expect_no_underflow();
    CHECK_EQ(sys_close(FIRST_USER_FD), 0);
    CHECK_EQ(g_node_refs[0], 0);
    expect_no_underflow();

    TEST_REPORT("fd-extreme-ubsan");
}
