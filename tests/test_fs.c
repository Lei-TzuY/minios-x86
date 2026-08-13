#include "../fs.h"
#include "../utils.h"

#include "test.h"

/*
 * The VFS core (fs.c). Every path-taking syscall funnels through exactly two
 * functions here -- resolve_fs() for "find this object" and resolve_parent_fs()
 * for "find the directory that owns this name" -- plus the six dispatch
 * wrappers. Until now only vfs_resolve_path(), the *normaliser*, had a unit
 * test (tests/test_fs_path.c); the strict resolvers that consume its output
 * had none, and were covered only incidentally by whatever paths the shell
 * happens to type.
 *
 * Three reasons this layer is worth pinning down directly:
 *
 *   - F8 lived here. An over-long path was silently truncated and then
 *     resolved to an ancestor directory, so a caller asking about "sub"
 *     operated on the cwd instead. The failure mode of a path resolver is not
 *     a crash, it is quietly returning the WRONG object -- which is why these
 *     tests assert which node an operation was dispatched to and with what
 *     name, not merely that it succeeded or failed.
 *
 *   - The two resolvers must agree with the normaliser. vfs_resolve_path()
 *     produces the path; resolve_fs() and resolve_parent_fs() parse it again
 *     with independent rules (component limits, "//", trailing slash, "." and
 *     ".."). A disagreement is either a spurious ENOENT or, in the dangerous
 *     direction, a path the normaliser truncated and the resolver accepted.
 *     test_resolver_agreement() checks that direction on a corpus.
 *
 *   - Every rejection is defence in depth for a backend. finddir/create/unlink
 *     are function pointers into ramfs, diskfs, fat16 or procfs; fs.c decides
 *     what those backends are ever asked to do.
 *
 * The filesystem under the resolvers is a mock, not RAMFS. Linking ramfs.c
 * would mostly test ramfs's own private resolver, and worse, ramfs validates
 * names itself -- an over-strict backend hides which layer did the rejecting
 * (the CAP10 lesson). This mock is deliberately permissive: it will happily
 * return a child literally named ".", ".." or "", and one file node carries a
 * full set of directory operations. Nothing a real backend would accept -- so
 * when a bad component never reaches it, that is fs.c doing the work, and the
 * call counters prove it rather than a lucky NULL from a lookup that simply
 * matched nothing.
 */

/* --- mock filesystem ------------------------------------------------------ */

#define MOCK_MAX 24
#define NAME_SPY 192      /* larger than fs_node_t::name, to catch overruns */

static fs_node_t  mock_nodes[MOCK_MAX];
static fs_node_t *mock_parent[MOCK_MAX];
static int        mock_count;

/* Observations. Every mock operation records that it ran, on what, and with
 * which arguments; the tests assert on these rather than on return values
 * alone, because "resolved to the wrong directory" is invisible otherwise. */
static int         g_finddir_calls;
static int         g_readdir_calls;
static int         g_read_calls;
static int         g_write_calls;
static int         g_open_calls;
static int         g_close_calls;
static int         g_create_calls;
static int         g_unlink_calls;
static int         g_mkdir_calls;
static int         g_rmdir_calls;

static char        g_last_lookup[NAME_SPY];   /* name passed to finddir */
static fs_node_t  *g_last_parent;             /* node an op was dispatched on */
static char        g_last_name[NAME_SPY];     /* name an op was dispatched with */
static fs_node_t  *g_last_rw_node;
static uint32_t    g_last_offset;
static uint32_t    g_last_size;
static uint8_t    *g_last_buffer;
static uint32_t    g_last_index;

/* Configurable backend answers, so the tests can prove fs.c forwards them
 * verbatim instead of inventing its own success or failure. */
static uint32_t    g_rw_result   = 7;
static int         g_op_result   = -1;
static fs_node_t  *g_create_result;

static dirent_t    mock_dirent;

static void reset_observations(void) {
    g_finddir_calls = g_readdir_calls = g_read_calls = g_write_calls = 0;
    g_open_calls = g_close_calls = 0;
    g_create_calls = g_unlink_calls = g_mkdir_calls = g_rmdir_calls = 0;
    g_last_parent = NULL;
    g_last_rw_node = NULL;
    g_last_offset = g_last_size = g_last_index = 0;
    g_last_buffer = NULL;

    /* Poison the name spies. resolve_parent_fs() writes the final component
     * into a caller-supplied buffer, and that string is the only thing the
     * backend ever sees; if it were left unterminated the poison shows up in
     * the comparison instead of reading as a lucky empty string. */
    for (int i = 0; i < NAME_SPY; i++) {
        g_last_lookup[i] = (char)0xAA;
        g_last_name[i] = (char)0xAA;
    }
}

/* Bounded copy of a backend-visible name into a spy buffer. Bounded on
 * purpose: an unterminated name must surface as a wrong string, not as a
 * crash in the test harness. */
static void spy_copy(char *dest, const char *src) {
    int i = 0;

    while (i < NAME_SPY - 1 && src[i]) { dest[i] = src[i]; i++; }
    dest[i] = '\0';
}

static uint32_t mock_read(fs_node_t *node, uint32_t offset, uint32_t size,
                          uint8_t *buffer) {
    g_read_calls++;
    g_last_rw_node = node;
    g_last_offset = offset;
    g_last_size = size;
    g_last_buffer = buffer;
    return g_rw_result;
}

static uint32_t mock_write(fs_node_t *node, uint32_t offset, uint32_t size,
                           uint8_t *buffer) {
    g_write_calls++;
    g_last_rw_node = node;
    g_last_offset = offset;
    g_last_size = size;
    g_last_buffer = buffer;
    return g_rw_result;
}

static void mock_open(fs_node_t *node)  { g_open_calls++;  g_last_rw_node = node; }
static void mock_close(fs_node_t *node) { g_close_calls++; g_last_rw_node = node; }

static dirent_t *mock_readdir(fs_node_t *node, uint32_t index) {
    g_readdir_calls++;
    g_last_rw_node = node;
    g_last_index = index;
    return index == 0 ? &mock_dirent : NULL;
}

/* Exact-match lookup and nothing else: no name validation, so a rejected
 * component proves fs.c rejected it. */
static fs_node_t *mock_finddir(fs_node_t *dir, const char *name) {
    g_finddir_calls++;
    spy_copy(g_last_lookup, name);

    for (int i = 0; i < mock_count; i++) {
        if (mock_parent[i] == dir && strcmp(mock_nodes[i].name, name) == 0)
            return &mock_nodes[i];
    }
    return NULL;
}

static fs_node_t *mock_create(fs_node_t *parent, const char *name) {
    g_create_calls++;
    g_last_parent = parent;
    spy_copy(g_last_name, name);
    return g_create_result;
}

static int mock_unlink(fs_node_t *parent, const char *name) {
    g_unlink_calls++;
    g_last_parent = parent;
    spy_copy(g_last_name, name);
    return g_op_result;
}

static int mock_mkdir(fs_node_t *parent, const char *name) {
    g_mkdir_calls++;
    g_last_parent = parent;
    spy_copy(g_last_name, name);
    return g_op_result;
}

static int mock_rmdir(fs_node_t *parent, const char *name) {
    g_rmdir_calls++;
    g_last_parent = parent;
    spy_copy(g_last_name, name);
    return g_op_result;
}

/* --- tree construction ---------------------------------------------------- */

static fs_node_t *add_node(fs_node_t *parent, const char *name, uint32_t flags) {
    fs_node_t *node;

    if (mock_count >= MOCK_MAX) return NULL;
    node = &mock_nodes[mock_count];
    memset(node, 0, sizeof(*node));
    strcpy(node->name, name);
    node->flags = flags;
    node->inode = (uint32_t)mock_count + 1;

    if (flags == FS_DIRECTORY) {
        node->readdir = mock_readdir;
        node->finddir = mock_finddir;
        node->create  = mock_create;
        node->unlink  = mock_unlink;
        node->mkdir   = mock_mkdir;
        node->rmdir   = mock_rmdir;
    } else {
        node->read  = mock_read;
        node->write = mock_write;
        node->open  = mock_open;
        node->close = mock_close;
    }

    mock_parent[mock_count] = parent;
    mock_count++;
    return node;
}

/* A directory that offers lookup but none of the mutating operations -- the
 * shape procfs has. fs.c must turn that into a clean failure, not a call
 * through a NULL pointer. */
static fs_node_t *add_readonly_dir(fs_node_t *parent, const char *name) {
    fs_node_t *node = add_node(parent, name, FS_DIRECTORY);

    if (node) {
        node->create = NULL;
        node->unlink = NULL;
        node->mkdir  = NULL;
        node->rmdir  = NULL;
    }
    return node;
}

/* The tree every test runs against:
 *
 *   /            root        (dir)
 *     a          dir
 *       b        dir
 *         c      file
 *       .        file, literally named "."     -- must never be looked up
 *       ..       file, literally named ".."    -- must never be looked up
 *     f          file
 *     ro         dir with no create/unlink/mkdir/rmdir
 *     odd        FILE that wrongly exposes every directory operation
 *       deep     file below a non-directory
 *     ""         dir literally named "" -- must never be looked up
 *       x        file below it
 */
static fs_node_t *root, *dir_a, *dir_b, *file_c, *file_f, *dir_ro, *odd, *deep;
static fs_node_t *dot_named, *dotdot_named, *empty_named, *empty_child;

static void build_tree(void) {
    mock_count = 0;
    root  = add_node(NULL, "root", FS_DIRECTORY);
    fs_root = root;

    dir_a  = add_node(root,  "a", FS_DIRECTORY);
    dir_b  = add_node(dir_a, "b", FS_DIRECTORY);
    file_c = add_node(dir_b, "c", FS_FILE);
    file_f = add_node(dir_a, "f", FS_FILE);
    dir_ro = add_readonly_dir(root, "ro");

    /* Nodes whose names are exactly the components fs.c must refuse to look
     * up. A backend cannot normally hold these, which is the point: if fs.c
     * ever asks for one, the mock answers and the test fails. Without them the
     * lookup returns NULL for the boring reason (nothing matches) and a
     * missing check passes unnoticed -- the CAP10 masking pattern. */
    dot_named    = add_node(dir_a, ".",  FS_FILE);
    dotdot_named = add_node(dir_a, "..", FS_FILE);
    empty_named  = add_node(root,  "",   FS_DIRECTORY);
    empty_child  = add_node(empty_named, "x", FS_FILE);

    /* A file node carrying a full set of directory operations. No shipping
     * backend builds one, and that is exactly why it belongs here: it isolates
     * fs.c's own "is this a directory?" checks from the accident that a real
     * file happens to have NULL function pointers. */
    odd  = add_node(root, "odd", FS_FILE);
    odd->finddir = mock_finddir;
    odd->create  = mock_create;
    odd->unlink  = mock_unlink;
    odd->mkdir   = mock_mkdir;
    odd->rmdir   = mock_rmdir;
    deep = add_node(odd, "deep", FS_FILE);

    (void)dot_named;
    (void)dotdot_named;
    (void)empty_child;
    (void)deep;
}

/* --- dispatch wrappers ---------------------------------------------------- */

static void test_dispatch_null_safe(void) {
    uint8_t buffer[4];
    fs_node_t empty;

    TEST("dispatch null-safe");
    memset(&empty, 0, sizeof(empty));      /* every op pointer NULL */
    reset_observations();

    /* A NULL node reaches these from every "file was not found" path; a node
     * with a NULL op is normal (directories have no read, files no readdir).
     * Neither may be dereferenced, and both must yield the empty answer. */
    CHECK_EQ(read_fs(NULL, 0, sizeof(buffer), buffer), 0);
    CHECK_EQ(read_fs(&empty, 0, sizeof(buffer), buffer), 0);
    CHECK_EQ(write_fs(NULL, 0, sizeof(buffer), buffer), 0);
    CHECK_EQ(write_fs(&empty, 0, sizeof(buffer), buffer), 0);
    CHECK(readdir_fs(NULL, 0) == NULL);
    CHECK(readdir_fs(&empty, 0) == NULL);
    CHECK(finddir_fs(NULL, "x") == NULL);
    CHECK(finddir_fs(&empty, "x") == NULL);

    open_fs(NULL);
    open_fs(&empty);
    close_fs(NULL);
    close_fs(&empty);

    /* Nothing above may have reached a backend. */
    CHECK_EQ(g_read_calls, 0);
    CHECK_EQ(g_write_calls, 0);
    CHECK_EQ(g_readdir_calls, 0);
    CHECK_EQ(g_finddir_calls, 0);
    CHECK_EQ(g_open_calls, 0);
    CHECK_EQ(g_close_calls, 0);
}

static void test_dispatch_forwards(void) {
    uint8_t buffer[8];

    TEST("dispatch forwards");
    build_tree();
    reset_observations();

    /* Arguments must arrive unchanged -- an offset or size mangled here would
     * corrupt every filesystem at once. */
    g_rw_result = 5;
    CHECK_EQ(read_fs(file_c, 11, 8, buffer), 5);
    CHECK_EQ(g_read_calls, 1);
    CHECK(g_last_rw_node == file_c);
    CHECK_EQ(g_last_offset, 11);
    CHECK_EQ(g_last_size, 8);
    CHECK(g_last_buffer == buffer);

    g_rw_result = 3;
    CHECK_EQ(write_fs(file_c, 4096, 2, buffer), 3);
    CHECK_EQ(g_write_calls, 1);
    CHECK(g_last_rw_node == file_c);
    CHECK_EQ(g_last_offset, 4096);
    CHECK_EQ(g_last_size, 2);
    CHECK(g_last_buffer == buffer);

    /* A zero-length read must still be forwarded, not short-circuited: only
     * the backend knows whether that is an error. */
    g_rw_result = 0;
    CHECK_EQ(read_fs(file_c, 0, 0, buffer), 0);
    CHECK_EQ(g_read_calls, 2);

    open_fs(file_c);
    CHECK_EQ(g_open_calls, 1);
    CHECK(g_last_rw_node == file_c);
    close_fs(file_c);
    CHECK_EQ(g_close_calls, 1);
    CHECK(g_last_rw_node == file_c);

    /* Reference counting is what makes "open files cannot be unlinked" work
     * (F11/F20), so an open that silently does not reach the backend is a
     * memory-safety bug, not a cosmetic one. */
    open_fs(file_c);
    open_fs(file_c);
    CHECK_EQ(g_open_calls, 3);

    CHECK(readdir_fs(dir_a, 0) == &mock_dirent);
    CHECK(readdir_fs(dir_a, 1) == NULL);
    CHECK_EQ(g_readdir_calls, 2);
    CHECK_EQ(g_last_index, 1);
    CHECK(g_last_rw_node == dir_a);

    CHECK(finddir_fs(dir_a, "b") == dir_b);
    CHECK_STREQ(g_last_lookup, "b");
    CHECK(finddir_fs(dir_a, "nope") == NULL);
    CHECK_EQ(g_finddir_calls, 2);

    g_rw_result = 7;
}

/* --- resolve_fs ----------------------------------------------------------- */

static void test_resolve_basic(void) {
    TEST("resolve basic");
    build_tree();
    reset_observations();

    CHECK(resolve_fs("/") == root);
    CHECK_EQ(g_finddir_calls, 0);          /* the root needs no lookup */

    CHECK(resolve_fs("/a") == dir_a);
    CHECK(resolve_fs("/a/b") == dir_b);
    CHECK(resolve_fs("/a/b/c") == file_c);
    CHECK(resolve_fs("/a/f") == file_f);

    /* No leading slash: resolution starts at the root either way. This is the
     * form elf_loader.c uses for a program name, so it is not hypothetical. */
    CHECK(resolve_fs("a/b/c") == file_c);

    CHECK(resolve_fs("/a/missing") == NULL);
    CHECK(resolve_fs("/missing/b") == NULL);

    /* Stop at the first failure: continuing to walk after a miss is how a
     * resolver ends up answering with a node from the wrong subtree. */
    reset_observations();
    CHECK(resolve_fs("/missing/b/c/d") == NULL);
    CHECK_EQ(g_finddir_calls, 1);

    /* A file is a valid answer, but not a valid intermediate. */
    reset_observations();
    CHECK(resolve_fs("/a/f/x") == NULL);
    CHECK_EQ(g_finddir_calls, 2);
}

static void test_resolve_rejects_syntax(void) {
    TEST("resolve syntax");
    build_tree();
    reset_observations();

    CHECK(resolve_fs(NULL) == NULL);

    /* "." and ".." are the normaliser's job (vfs_resolve_path collapses them);
     * the strict resolver refuses them outright. The mock tree contains nodes
     * LITERALLY named "." and ".." as children of /a, so if fs.c ever passed
     * such a component through, the lookup would succeed and this would fail.
     * That is the point: the rejection must be fs.c's, not the backend's. */
    CHECK(resolve_fs("/a/.") == NULL);
    CHECK(resolve_fs("/a/..") == NULL);
    CHECK(resolve_fs("/a/./b") == NULL);
    CHECK(resolve_fs("/a/../a") == NULL);
    CHECK_EQ(g_finddir_calls, 4);          /* "a" only, four times */
    CHECK_STREQ(g_last_lookup, "a");

    /* Empty components. A trailing slash is rejected rather than treated as
     * "the same directory", so exactly one spelling reaches a backend. */
    CHECK(resolve_fs("/a/") == NULL);
    CHECK(resolve_fs("/a/b/") == NULL);
    CHECK(resolve_fs("//") == NULL);
    CHECK(resolve_fs("/a//b") == NULL);
    CHECK(resolve_fs("/a/b//") == NULL);

    /* A leading "//" is the one place an empty component reaches the parser
     * with path left to walk: the leading-slash skip consumes the first, the
     * second opens a zero-length component. Every other "//" is caught by the
     * explicit double-slash test, so this is the only input that isolates
     * parse_component's own empty-component check -- and the mock has a
     * directory named "" holding a child, so a component that got through
     * would resolve to it instead of failing for want of a match. */
    reset_observations();
    CHECK(resolve_fs("//x") == NULL);
    CHECK(resolve_fs("//x/y") == NULL);
    CHECK_EQ(g_finddir_calls, 0);
}

static void test_resolve_limits(void) {
    char path[FS_MAX_PATH + 64];
    char relative[FS_MAX_PATH + 64];
    char longname[FS_MAX_PATH];
    fs_node_t *deep_node;
    int i;

    TEST("resolve limits");
    build_tree();

    /* An over-long path is refused before any lookup happens. Asserting the
     * call count -- not just the NULL -- is what makes this test meaningful:
     * the point of the length check is that a 300-byte path never drives the
     * backend at all (cf. the e_phnum bound in tests/test_elf.c). */
    path[0] = '/';
    for (i = 1; i < FS_MAX_PATH + 40; i++) path[i] = 'z';
    path[FS_MAX_PATH + 40] = '\0';
    reset_observations();
    CHECK(resolve_fs(path) == NULL);
    CHECK_EQ(g_finddir_calls, 0);

    /* Exactly FS_MAX_PATH characters (no room for the terminator) is out; one
     * shorter is in. resolve_fs must accept everything vfs_resolve_path can
     * hand it, so this boundary is load-bearing, not decorative. */
    for (i = 1; i < FS_MAX_PATH; i++) path[i] = 'z';
    path[FS_MAX_PATH] = '\0';
    reset_observations();
    CHECK_EQ(strlen(path), FS_MAX_PATH);
    CHECK(resolve_fs(path) == NULL);
    CHECK_EQ(g_finddir_calls, 0);

    path[FS_MAX_PATH - 1] = '\0';
    CHECK_EQ(strlen(path), FS_MAX_PATH - 1);
    reset_observations();
    CHECK(resolve_fs(path) == NULL);       /* no such node ... */
    CHECK_EQ(g_finddir_calls, 1);          /* ... but it WAS looked up */

    /* The longest component reachable through an absolute path: "/" plus
     * FS_MAX_PATH-2 characters is FS_MAX_PATH-1 long, the most that fits.
     * Built as a real node so the success case is proven, not assumed. */
    for (i = 0; i < FS_MAX_PATH - 2; i++) longname[i] = 'n';
    longname[FS_MAX_PATH - 2] = '\0';
    CHECK_EQ(strlen(longname), FS_MAX_PATH - 2);
    deep_node = add_node(root, longname, FS_FILE);
    path[0] = '/';
    strcpy(path + 1, longname);
    reset_observations();
    CHECK(resolve_fs(path) == deep_node);
    CHECK_EQ(g_finddir_calls, 1);

    /* One character longer -- the longest name fs_node_t::name can store,
     * 127 characters plus its terminator. It no longer fits in any absolute
     * path (that would be FS_MAX_PATH bytes), but it does fit as a relative
     * one, which is exactly how exec resolves a program name. So such a node
     * is reachable, and the resolver must reach it. */
    for (i = 0; i < FS_MAX_PATH - 1; i++) relative[i] = 'r';
    relative[FS_MAX_PATH - 1] = '\0';
    CHECK_EQ(strlen(relative), FS_MAX_PATH - 1);
    deep_node = add_node(root, relative, FS_FILE);
    CHECK_EQ(strlen(deep_node->name), FS_MAX_PATH - 1);
    reset_observations();
    CHECK(resolve_fs(relative) == deep_node);
    CHECK_EQ(g_finddir_calls, 1);

    /* The same name with a leading slash is one byte too long for a path and
     * is refused before any lookup. */
    path[0] = '/';
    strcpy(path + 1, relative);
    CHECK_EQ(strlen(path), FS_MAX_PATH);
    reset_observations();
    CHECK(resolve_fs(path) == NULL);
    CHECK_EQ(g_finddir_calls, 0);

    /* A component of FS_MAX_PATH characters -- one more than a name can hold.
     * parse_component has its own guard for this, but the path-length check
     * above always fires first (a component that long makes the path at least
     * that long), so the guard is unreachable defence in depth rather than a
     * live check. Recorded here honestly: the assertion below passes either
     * way, and no test is written to pretend otherwise. */
    for (i = 0; i < FS_MAX_PATH; i++) relative[i] = 'r';
    relative[FS_MAX_PATH] = '\0';
    CHECK_EQ(strlen(relative), FS_MAX_PATH);
    reset_observations();
    CHECK(resolve_fs(relative) == NULL);
    CHECK_EQ(g_finddir_calls, 0);
}

static void test_resolve_requires_directory(void) {
    TEST("resolve intermediate is a directory");
    build_tree();
    reset_observations();

    /* /odd is a FILE that nevertheless exposes a finddir. No shipping backend
     * builds such a node, but procfs's finddir ignores its node argument
     * entirely, so "the backend will refuse" is not a property fs.c can lean
     * on. resolve_parent_fs has always checked the type of an intermediate;
     * resolve_fs must too, or the same path means two different things
     * depending on which entry point the caller used. */
    CHECK(resolve_fs("/odd") == odd);      /* as a final component: fine */
    CHECK(resolve_fs("/odd/deep") == NULL);
    CHECK_EQ(unlink_fs("/odd/deep"), -1);
    CHECK_EQ(g_unlink_calls, 0);
}

static void test_no_root(void) {
    fs_node_t *saved;

    TEST("no root mounted");
    build_tree();
    saved = fs_root;
    fs_root = NULL;
    reset_observations();

    /* Before ramfs_init() runs, and in any future teardown path, fs_root is
     * NULL. Nothing may be dereferenced. */
    CHECK(resolve_fs("/") == NULL);
    CHECK(resolve_fs("/a") == NULL);
    CHECK(create_fs("/a") == NULL);
    CHECK_EQ(unlink_fs("/a"), -1);
    CHECK_EQ(mkdir_fs("/a"), -1);
    CHECK_EQ(rmdir_fs("/a"), -1);
    CHECK_EQ(g_finddir_calls, 0);
    CHECK_EQ(g_create_calls, 0);

    fs_root = saved;
}

/* --- resolve_parent_fs, via the four mutating entry points ---------------- */

static void test_parent_dispatch(void) {
    TEST("parent dispatch");
    build_tree();
    reset_observations();

    /* The final component is split off and handed to the OWNING directory.
     * Checking the parent pointer AND the name is the whole point: F8 was a
     * case where both were individually plausible and jointly wrong. */
    g_create_result = file_f;
    CHECK(create_fs("/a/new") == file_f);
    CHECK_EQ(g_create_calls, 1);
    CHECK(g_last_parent == dir_a);
    CHECK_STREQ(g_last_name, "new");

    CHECK(create_fs("/a/b/new") == file_f);
    CHECK(g_last_parent == dir_b);
    CHECK_STREQ(g_last_name, "new");

    /* A name directly under the root: the parent is the root itself. */
    CHECK(create_fs("/top") == file_f);
    CHECK(g_last_parent == root);
    CHECK_STREQ(g_last_name, "top");

    /* Relative spelling resolves against the root, same as resolve_fs. */
    CHECK(create_fs("a/rel") == file_f);
    CHECK(g_last_parent == dir_a);
    CHECK_STREQ(g_last_name, "rel");

    /* Backend answers are forwarded verbatim, including failure. */
    g_create_result = NULL;
    CHECK(create_fs("/a/new") == NULL);

    g_op_result = 0;
    CHECK_EQ(unlink_fs("/a/b/c"), 0);
    CHECK_EQ(g_unlink_calls, 1);
    CHECK(g_last_parent == dir_b);
    CHECK_STREQ(g_last_name, "c");

    CHECK_EQ(mkdir_fs("/a/sub"), 0);
    CHECK_EQ(g_mkdir_calls, 1);
    CHECK(g_last_parent == dir_a);
    CHECK_STREQ(g_last_name, "sub");

    CHECK_EQ(rmdir_fs("/a/b"), 0);
    CHECK_EQ(g_rmdir_calls, 1);
    CHECK(g_last_parent == dir_a);
    CHECK_STREQ(g_last_name, "b");

    g_op_result = -1;
    CHECK_EQ(unlink_fs("/a/b/c"), -1);
    CHECK_EQ(mkdir_fs("/a/sub"), -1);
    CHECK_EQ(rmdir_fs("/a/b"), -1);
}

static void test_parent_rejects(void) {
    TEST("parent rejects");
    build_tree();
    reset_observations();
    g_create_result = file_f;
    g_op_result = 0;

    /* The root has no parent, so there is no name to remove or create. Each of
     * these must fail WITHOUT reaching a backend -- an rmdir("/") that got
     * through to ramfs would try to free the root node. */
    CHECK(create_fs("/") == NULL);
    CHECK(create_fs("") == NULL);
    CHECK_EQ(unlink_fs("/"), -1);
    CHECK_EQ(unlink_fs(""), -1);
    CHECK_EQ(mkdir_fs("/"), -1);
    CHECK_EQ(rmdir_fs("/"), -1);
    CHECK(create_fs(NULL) == NULL);
    CHECK_EQ(unlink_fs(NULL), -1);

    /* Trailing slash, empty component, "." and "..": same rules as resolve_fs,
     * so that one object has exactly one spelling at both entry points. */
    CHECK(create_fs("/a/") == NULL);
    CHECK(create_fs("/a//b") == NULL);
    CHECK(create_fs("/a/./b") == NULL);
    CHECK(create_fs("/a/.") == NULL);
    CHECK(create_fs("/a/..") == NULL);
    CHECK_EQ(rmdir_fs("/a/b/"), -1);

    /* A leading "//" -- see test_resolve_rejects_syntax. The mock's directory
     * named "" would accept the operation if the empty component ever got
     * through, so this isolates the check instead of relying on a miss. */
    CHECK(create_fs("//x") == NULL);
    CHECK_EQ(mkdir_fs("//x"), -1);

    /* Nothing so far may have reached a backend. */
    CHECK_EQ(g_create_calls, 0);
    CHECK_EQ(g_unlink_calls, 0);
    CHECK_EQ(g_mkdir_calls, 0);
    CHECK_EQ(g_rmdir_calls, 0);

    /* A malformed path stops the walk where the malformation is, rather than
     * failing later on for an unrelated reason. Both spellings below end in
     * NULL either way, so the returned value proves nothing; the lookup count
     * is what distinguishes "refused this separator" from "wandered one
     * directory further and then gave up". It is worth pinning: on the
     * disk-backed filesystems a lookup is ATA PIO with interrupts disabled,
     * so an early refusal is a cheap one. */
    reset_observations();
    CHECK(create_fs("/a//b") == NULL);
    CHECK_EQ(mkdir_fs("/a//b"), -1);
    CHECK_EQ(g_finddir_calls, 0);

    reset_observations();
    CHECK_EQ(unlink_fs("/a/b//c"), -1);
    CHECK_EQ(g_finddir_calls, 1);      /* "a" resolved, then the walk stopped */
    reset_observations();

    /* Missing or non-directory parent. Two shapes, because they exercise
     * different guards: /a/f is a file with NULL operations (so a missing type
     * check would still fail, for the wrong reason), while /odd is a file that
     * carries a full set of operations -- only the type check stops it. */
    CHECK(create_fs("/missing/x") == NULL);
    CHECK(create_fs("/a/f/x") == NULL);
    CHECK_EQ(unlink_fs("/a/f/x"), -1);
    CHECK(create_fs("/odd/x") == NULL);
    CHECK_EQ(unlink_fs("/odd/x"), -1);
    CHECK_EQ(mkdir_fs("/odd/x"), -1);
    CHECK_EQ(rmdir_fs("/odd/x"), -1);

    CHECK_EQ(g_create_calls, 0);
    CHECK_EQ(g_unlink_calls, 0);
    CHECK_EQ(g_mkdir_calls, 0);
    CHECK_EQ(g_rmdir_calls, 0);

    /* An over-long path must not reach a backend either. */
    {
        char path[FS_MAX_PATH + 40];
        int i;

        path[0] = '/';
        for (i = 1; i < FS_MAX_PATH + 20; i++) path[i] = 'q';
        path[FS_MAX_PATH + 20] = '\0';
        CHECK(create_fs(path) == NULL);
        CHECK_EQ(unlink_fs(path), -1);
        CHECK_EQ(g_create_calls, 0);
        CHECK_EQ(g_unlink_calls, 0);
    }
}

static void test_parent_missing_ops(void) {
    TEST("parent without ops");
    build_tree();
    reset_observations();
    g_create_result = file_f;
    g_op_result = 0;

    /* /ro resolves fine but implements none of the four mutating operations,
     * which is the shape of procfs. fs.c must check each pointer before
     * calling it; the alternative is a jump through NULL in ring 0. */
    CHECK(resolve_fs("/ro") == dir_ro);
    CHECK(create_fs("/ro/x") == NULL);
    CHECK_EQ(unlink_fs("/ro/x"), -1);
    CHECK_EQ(mkdir_fs("/ro/x"), -1);
    CHECK_EQ(rmdir_fs("/ro/x"), -1);
    CHECK_EQ(g_create_calls, 0);
}

static void test_parent_name_is_terminated(void) {
    char longname[FS_MAX_PATH];
    char path[FS_MAX_PATH + 8];
    int i;

    TEST("parent name termination");
    build_tree();
    reset_observations();
    g_create_result = file_f;

    /* The longest component an absolute path can carry, created through the
     * longest path that still fits. The spy buffer is poisoned with 0xAA
     * before the call, so a name copied without its terminator reads back as
     * the name followed by poison and the comparison fails. */
    for (i = 0; i < FS_MAX_PATH - 2; i++) longname[i] = 'm';
    longname[FS_MAX_PATH - 2] = '\0';

    path[0] = '/';
    strcpy(path + 1, longname);
    CHECK_EQ(strlen(path), FS_MAX_PATH - 1);

    CHECK(create_fs(path) == file_f);
    CHECK_EQ(g_create_calls, 1);
    CHECK(g_last_parent == root);
    CHECK_STREQ(g_last_name, longname);
}

static void test_empty_path(void) {
    char out[FS_MAX_PATH];

    TEST("empty path resolves to the cwd");
    build_tree();
    reset_observations();

    /* POSIX would make open("") an ENOENT; here an empty path normalises to
     * the working directory. Pinned rather than changed, because every caller
     * that could act on the result is already safe: a cwd is always a
     * directory (sys_chdir enforces it), so sys_open rejects it for not being
     * FS_FILE, unlink and rmdir hand a directory name to a backend that
     * refuses it, and create finds the name already taken. Writing it down as
     * a test means a future change to any link in that chain has to be a
     * deliberate one rather than a surprise. */
    CHECK_EQ(vfs_resolve_path("/a/b", "", out), 0);
    CHECK_STREQ(out, "/a/b");
    CHECK(resolve_fs(out) == dir_b);

    CHECK_EQ(vfs_resolve_path("/", "", out), 0);
    CHECK_STREQ(out, "/");
    CHECK(resolve_fs(out) == root);

    /* The strict resolvers treat an empty path differently from each other,
     * and both are safe: resolve_fs answers with the root (it is the "walk
     * nothing" case), while resolve_parent_fs refuses, because a path with no
     * final component has no name to create or remove. */
    CHECK(resolve_fs("") == root);
    CHECK(create_fs("") == NULL);
    CHECK_EQ(unlink_fs(""), -1);
    CHECK_EQ(g_create_calls, 0);
    CHECK_EQ(g_unlink_calls, 0);
}

/* --- the normaliser and the resolvers must agree -------------------------- */

static void test_resolver_agreement(void) {
    /* Every (cwd, path) pair here names /a/b/c or one of its ancestors. The
     * property under test: whatever vfs_resolve_path() accepts, the strict
     * resolvers must also accept -- same object, no spurious rejection.
     *
     * The dangerous direction is the one F8 was: the normaliser quietly
     * shortening a path that the resolver then happily resolves to an
     * ancestor. Comparing the resolved node against the node the caller asked
     * for is what catches that; comparing only the returned string would not,
     * because a truncated path is still a well-formed path. */
    static const struct { const char *cwd; const char *path; } cases[] = {
        { "/",        "/a/b/c"        },
        { "/",        "a/b/c"         },
        { "/a",       "b/c"           },
        { "/a/b",     "c"             },
        { "/a/b",     "./c"           },
        { "/a/b/c",   "../c"          },
        { "/a/b",     "../b/c"        },
        { "/a/b",     "../../a/b/c"   },
        { "/a/b",     "/a/b/c"        },
        { "/a",       "./b/./c"       },
        { "/a/b",     "../b/../b/c"   },
        { "/ro",      "../a/b/c"      },
    };
    char out[FS_MAX_PATH];
    unsigned i;

    TEST("normaliser / resolver agreement");
    build_tree();

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        reset_observations();
        CHECK_EQ(vfs_resolve_path(cases[i].cwd, cases[i].path, out), 0);
        CHECK_STREQ(out, "/a/b/c");
        CHECK(resolve_fs(out) == file_c);

        /* The parent form has to agree too: same path, the owning directory
         * and the final name. */
        g_op_result = 0;
        CHECK_EQ(unlink_fs(out), 0);
        CHECK(g_last_parent == dir_b);
        CHECK_STREQ(g_last_name, "c");
    }

    /* Directory results, including the normalised root. */
    CHECK_EQ(vfs_resolve_path("/a/b", "..", out), 0);
    CHECK(resolve_fs(out) == dir_a);
    CHECK_EQ(vfs_resolve_path("/a/b", "../..", out), 0);
    CHECK_STREQ(out, "/");
    CHECK(resolve_fs(out) == root);

    /* And the length boundary, from both sides. vfs_resolve_path stops one
     * character short of what resolve_fs would accept, so its output is always
     * resolvable; the reverse (a normaliser that emitted a path the resolver
     * rejects) would strand callers with paths they can build but not use. */
    {
        char cwd[FS_MAX_PATH];
        int fill = FS_MAX_PATH - 3;        /* "/" + (fill-1) chars */
        int k;

        cwd[0] = '/';
        for (k = 1; k < fill; k++) cwd[k] = 'd';
        cwd[fill] = '\0';

        CHECK_EQ(vfs_resolve_path(cwd, ".", out), 0);
        CHECK_EQ(strlen(out), FS_MAX_PATH - 3);
        CHECK(strlen(out) < FS_MAX_PATH);      /* resolve_fs would take it */

        /* One more character still fits in both. */
        CHECK_EQ(vfs_resolve_path(cwd, "", out), 0);
        CHECK_EQ(strlen(out), FS_MAX_PATH - 3);

        /* Appending "/x" needs two more bytes: the result would be exactly
         * FS_MAX_PATH-1 characters, which resolve_fs accepts but the
         * normaliser refuses. Rejecting is the safe direction -- silently
         * dropping the "x" would hand back the parent directory (F8). */
        CHECK_EQ(vfs_resolve_path(cwd, "x", out), -1);
    }
}

int main(void) {
    test_dispatch_null_safe();
    test_dispatch_forwards();
    test_resolve_basic();
    test_resolve_rejects_syntax();
    test_resolve_limits();
    test_resolve_requires_directory();
    test_no_root();
    test_parent_dispatch();
    test_parent_rejects();
    test_parent_missing_ops();
    test_parent_name_is_terminated();
    test_empty_path();
    test_resolver_agreement();
    TEST_REPORT("fs-vfs");
}
