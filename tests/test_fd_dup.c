/* Focused ownership regressions for SYS_DUP.
 *
 * Reuse test_fdtable.c's reference-counted fake VFS/pipe objects and the real
 * syscall.c descriptor table.  Rename its main so this translation unit can
 * add a small feature-specific suite without duplicating that large harness.
 */
#define main fdtable_base_main
#include "test_fdtable.c"
#undef main

static void test_dup_file_uses_lowest_free_slot(void) {
    process_t *p;
    int source;
    int duplicate;

    TEST("dup chooses the lowest free descriptor and preserves offset");
    reset_world();
    p = take_slot(0, 10);
    g_current = p;

    CHECK_EQ(open_user_file(&g_nodes[0]), FIRST_USER_FD);
    CHECK_EQ(open_user_file(&g_nodes[1]), FIRST_USER_FD + 1);
    source = open_user_file(&g_nodes[2]);
    CHECK_EQ(source, FIRST_USER_FD + 2);
    open_files[0][2].offset = 1234;

    /* Free fd 3 while the source remains fd 5. dup must choose fd 3. */
    CHECK_EQ(sys_close(FIRST_USER_FD), 0);
    duplicate = sys_dup(source);
    CHECK_EQ(duplicate, FIRST_USER_FD);
    CHECK_EQ(open_files[0][0].kind, OF_FILE);
    CHECK(open_files[0][0].node == &g_nodes[2]);
    CHECK_EQ(open_files[0][0].offset, 1234);
    CHECK_EQ(g_node_refs[2], 2);

    /* The duplicate owns a real reference: closing the source leaves it live. */
    CHECK_EQ(sys_close(source), 0);
    CHECK_EQ(g_node_refs[2], 1);
    CHECK_EQ(open_files[0][0].kind, OF_FILE);
    CHECK(open_files[0][0].node == &g_nodes[2]);
    CHECK_EQ(sys_close(duplicate), 0);
    CHECK_EQ(g_node_refs[2], 0);
    expect_no_underflow();
}

static void test_dup_pipe_end_retains_matching_end(void) {
    process_t *p;
    open_file_t *files;
    pipe_t *pipe;
    int rfd, wfd, rdup, wdup;

    TEST("dup retains the exact pipe end it duplicates");
    reset_world();
    p = take_slot(0, 10);
    g_current = p;
    files = current_open_files();

    pipe = pipe_create();
    CHECK(pipe != NULL);
    rfd = alloc_fd(files, OF_PIPE_R, NULL, pipe);
    wfd = alloc_fd(files, OF_PIPE_W, NULL, pipe);
    CHECK_EQ(rfd, FIRST_USER_FD);
    CHECK_EQ(wfd, FIRST_USER_FD + 1);
    CHECK_EQ(g_pipe_read_refs[0], 1);
    CHECK_EQ(g_pipe_write_refs[0], 1);

    rdup = sys_dup(rfd);
    CHECK_EQ(rdup, FIRST_USER_FD + 2);
    CHECK_EQ(g_pipe_read_refs[0], 2);
    CHECK_EQ(g_pipe_write_refs[0], 1);

    wdup = sys_dup(wfd);
    CHECK_EQ(wdup, FIRST_USER_FD + 3);
    CHECK_EQ(g_pipe_read_refs[0], 2);
    CHECK_EQ(g_pipe_write_refs[0], 2);

    CHECK_EQ(sys_close(rfd), 0);
    CHECK_EQ(g_pipe_read_refs[0], 1);
    CHECK_EQ(g_pipe_write_refs[0], 2);
    CHECK_EQ(sys_close(wfd), 0);
    CHECK_EQ(g_pipe_read_refs[0], 1);
    CHECK_EQ(g_pipe_write_refs[0], 1);

    CHECK_EQ(sys_close(rdup), 0);
    CHECK_EQ(sys_close(wdup), 0);
    CHECK_EQ(total_pipe_refs(), 0);
    expect_no_underflow();
}

static void test_dup_full_table_changes_nothing(void) {
    process_t *p;
    int refs_before;

    TEST("dup on a full descriptor table fails without taking a reference");
    reset_world();
    p = take_slot(0, 10);
    g_current = p;

    for (int i = 0; i < MAX_OPEN_FILES; i++)
        CHECK_EQ(open_user_file(&g_nodes[i]), FIRST_USER_FD + i);
    refs_before = total_node_refs();

    CHECK_EQ(sys_dup(FIRST_USER_FD), -1);
    CHECK_EQ(total_node_refs(), refs_before);
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        CHECK_EQ(open_files[0][i].kind, OF_FILE);
        CHECK(open_files[0][i].node == &g_nodes[i]);
    }
    expect_no_underflow();
}

static void test_dup_rejects_invalid_sources(void) {
    process_t *p;

    TEST("dup rejects invalid, closed, and context-free descriptors");
    reset_world();
    p = take_slot(0, 10);
    g_current = p;
    CHECK_EQ(open_user_file(&g_nodes[0]), FIRST_USER_FD);

    CHECK_EQ(sys_dup(-1), -1);
    CHECK_EQ(sys_dup(0), -1);
    CHECK_EQ(sys_dup(FIRST_USER_FD + 1), -1); /* closed slot */
    CHECK_EQ(sys_dup(FIRST_USER_FD + MAX_OPEN_FILES), -1);
    CHECK_EQ(g_node_refs[0], 1);

    g_current = NULL;
    CHECK_EQ(sys_dup(FIRST_USER_FD), -1);
    CHECK_EQ(g_node_refs[0], 1);
    expect_no_underflow();
}

int main(void) {
    test_dup_file_uses_lowest_free_slot();
    test_dup_pipe_end_retains_matching_end();
    test_dup_full_table_changes_nothing();
    test_dup_rejects_invalid_sources();

    TEST_REPORT("fd dup");
}
