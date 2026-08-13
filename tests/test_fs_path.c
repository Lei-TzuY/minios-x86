#include "test.h"
#include "../fs.h"

/*
 * vfs_resolve_path() turns (cwd, path) into a clean absolute path. It is pure
 * string logic, and it is where a real bug lived: an over-long result used to
 * have its trailing component silently dropped while still reporting success,
 * so callers acted on an ancestor directory instead of the file they asked
 * for. The boundary between "just fits" and "one byte too long" is exactly
 * what the shell-driven suite cannot probe precisely, so pin it down here.
 */

/* The output buffer, with a canary immediately behind it.
 *
 * vfs_resolve_path's contract is that `out` holds FS_MAX_PATH bytes. A bound
 * that is one byte too generous writes the terminator at out[FS_MAX_PATH] --
 * and the return value still comes back as -1, because a second length check
 * at the end of the function rejects the result anyway. The two checks back
 * each other up, which is good for the kernel and bad for a test: the overflow
 * is invisible from the return value alone. The canary is what makes it
 * visible. */
static struct {
    char          path[FS_MAX_PATH];
    unsigned char guard[16];
} buf;

static char *const out = buf.path;

#define GUARD_BYTE 0x5A

static void arm_guard(void) {
    for (unsigned i = 0; i < sizeof(buf.guard); i++) buf.guard[i] = GUARD_BYTE;
}

static int guard_intact(void) {
    for (unsigned i = 0; i < sizeof(buf.guard); i++)
        if (buf.guard[i] != GUARD_BYTE) return 0;
    return 1;
}

static void test_absolute(void) {
    TEST("absolute");
    CHECK_EQ(vfs_resolve_path("/", "/a/b", out), 0);
    CHECK_STREQ(out, "/a/b");

    /* cwd is irrelevant for an absolute path. */
    CHECK_EQ(vfs_resolve_path("/x/y", "/a", out), 0);
    CHECK_STREQ(out, "/a");

    CHECK_EQ(vfs_resolve_path("/", "/", out), 0);
    CHECK_STREQ(out, "/");
}

static void test_relative(void) {
    TEST("relative");
    CHECK_EQ(vfs_resolve_path("/x", "y", out), 0);
    CHECK_STREQ(out, "/x/y");

    CHECK_EQ(vfs_resolve_path("/", "a", out), 0);
    CHECK_STREQ(out, "/a");

    CHECK_EQ(vfs_resolve_path("/a/b", "c/d", out), 0);
    CHECK_STREQ(out, "/a/b/c/d");
}

static void test_dot_and_dotdot(void) {
    TEST("dot / dotdot");
    CHECK_EQ(vfs_resolve_path("/a/b", ".", out), 0);
    CHECK_STREQ(out, "/a/b");

    CHECK_EQ(vfs_resolve_path("/a/b", "..", out), 0);
    CHECK_STREQ(out, "/a");

    CHECK_EQ(vfs_resolve_path("/a/b", "../..", out), 0);
    CHECK_STREQ(out, "/");

    /* Climbing past the root must clamp there, not underflow. */
    CHECK_EQ(vfs_resolve_path("/", "../../..", out), 0);
    CHECK_STREQ(out, "/");

    CHECK_EQ(vfs_resolve_path("/a", "./b/../c", out), 0);
    CHECK_STREQ(out, "/a/c");

    /* Trailing slash and repeated single slashes collapse. */
    CHECK_EQ(vfs_resolve_path("/", "/a/b/", out), 0);
    CHECK_STREQ(out, "/a/b");
}

static void test_rejects_empty_component(void) {
    TEST("reject //");
    CHECK_EQ(vfs_resolve_path("/", "/a//b", out), -1);
    CHECK_EQ(vfs_resolve_path("/", "//", out), -1);
}

static void test_length_boundary(void) {
    /* Build a cwd of exactly FS_MAX_PATH-2 characters: "/" + (N-2) 'd's.
     * Appending "/x" then needs two more bytes plus the terminator, which
     * cannot fit -- the resolver must fail rather than quietly hand back the
     * cwd (which is what callers would then operate on). */
    char cwd[FS_MAX_PATH];
    int fill = FS_MAX_PATH - 2;
    int i;

    TEST("length boundary");
    cwd[0] = '/';
    for (i = 1; i < fill; i++) cwd[i] = 'd';
    cwd[fill] = '\0';

    CHECK_EQ(vfs_resolve_path(cwd, "x", out), -1);

    /* ... and it must reject without having written anything past the buffer
     * on the way there. This cwd is 126 characters, so appending "/x" lands
     * the terminator exactly one byte past the end: the case a too-generous
     * bound would let through. */
    CHECK(guard_intact());

    /* The same cwd used on its own still resolves (nothing to append). */
    CHECK_EQ(vfs_resolve_path(cwd, ".", out), 0);
    CHECK_STREQ(out, cwd);

    /* A short cwd leaves room, so the identical operation succeeds -- this
     * guards against "fix" that simply rejects everything. */
    CHECK_EQ(vfs_resolve_path("/a", "x", out), 0);
    CHECK_STREQ(out, "/a/x");

    /* An absolute path that is itself too long is rejected too. */
    {
        char longp[FS_MAX_PATH + 40];
        longp[0] = '/';
        for (i = 1; i < FS_MAX_PATH + 20; i++) longp[i] = 'z';
        longp[FS_MAX_PATH + 20] = '\0';
        CHECK_EQ(vfs_resolve_path("/", longp, out), -1);
    }
}

static void test_bad_args(void) {
    TEST("bad args");
    CHECK_EQ(vfs_resolve_path("/", NULL, out), -1);
    CHECK_EQ(vfs_resolve_path("/", "a", NULL), -1);
}

int main(void) {
    arm_guard();
    test_absolute();
    test_relative();
    test_dot_and_dotdot();
    test_rejects_empty_component();
    test_length_boundary();
    test_bad_args();

    /* Nothing in the whole suite may have written past the buffer. */
    TEST("output buffer intact");
    CHECK(guard_intact());
    TEST_REPORT("fs-path");
}
