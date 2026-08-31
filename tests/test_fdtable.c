#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>

/*
 * The per-process descriptor table (syscall.c) and the invariant that spans it
 * and process.c:
 *
 *     every slot handed to a new process must come with an empty open_files[]
 *     table; no file or pipe reference may survive a slot being reused.
 *
 * That sentence is not written down anywhere in the code. process.c allocates a
 * slot with memset(process_t) and never touches open_files[]; correctness rests
 * entirely on every path that releases a slot having closed its descriptors
 * first. There are seven process_release() call sites, and the audit that
 * preceded this file confirmed all seven are currently correct -- four reap a
 * zombie that has already been through process_finish_exit, one follows it
 * directly, one closes the child's files explicitly on a failed fork, and one
 * fires before any descriptor could exist. An eighth added later that forgets
 * would hand a fresh process the previous occupant's file references, which is
 * F11's failure mode: a node whose refcount never drops, then a node freed
 * while a descriptor still points at it.
 *
 * So the point of this suite is ownership, not return values. Every stub below
 * keeps a real reference count, and a close with no matching open is recorded
 * as an underflow rather than being silently absorbed -- that is what makes a
 * double release visible. A test that only checked what sys_close() returned
 * would pass with every one of the mutants this file is written against.
 *
 * syscall.c is large and coupled; --gc-sections drops everything unreferenced,
 * so the stub surface is only what the descriptor paths actually reach. The
 * table is driven through open_user_file()/sys_close()/sys_dup2()/sys_pipe()
 * and the two lifecycle helpers, rather than through sys_open(), because
 * sys_open() adds path resolution and user-pointer validation without adding
 * anything to the ownership question.
 */

#include "../fs.h"
#include "../pipe.h"

/* --- reference-counted fake objects --------------------------------------- */

#define MAX_FAKE_NODES 8
#define MAX_FAKE_PIPES 4

static fs_node_t g_nodes[MAX_FAKE_NODES];
static int       g_node_refs[MAX_FAKE_NODES];
static int       g_node_underflow;      /* close with no matching open */

/* Pipes are referenced by end: pipe.c counts readers and writers separately,
 * and conflating them is one of the regressions this suite is written to
 * catch, so the model keeps them apart too. */
static pipe_t g_pipes[MAX_FAKE_PIPES];
static int    g_pipe_read_refs[MAX_FAKE_PIPES];
static int    g_pipe_write_refs[MAX_FAKE_PIPES];
static int    g_pipe_underflow;
static int    g_pipe_created;
static int    g_pipe_create_fails;

static int node_index(const fs_node_t *node) {
    for (int i = 0; i < MAX_FAKE_NODES; i++)
        if (&g_nodes[i] == node) return i;
    return -1;
}

static int pipe_index(const pipe_t *p) {
    for (int i = 0; i < MAX_FAKE_PIPES; i++)
        if (&g_pipes[i] == p) return i;
    return -1;
}

void open_fs(fs_node_t *node) {
    int i = node_index(node);
    if (i >= 0) g_node_refs[i]++;
}

void close_fs(fs_node_t *node) {
    int i = node_index(node);
    if (i < 0) return;
    if (g_node_refs[i] == 0) { g_node_underflow++; return; }
    g_node_refs[i]--;
}

pipe_t *pipe_create(void) {
    if (g_pipe_create_fails) return NULL;
    if (g_pipe_created >= MAX_FAKE_PIPES) return NULL;
    {
        pipe_t *p = &g_pipes[g_pipe_created];
        /* A fresh pipe starts with one reader and one writer, as pipe.c's
         * does: sys_pipe hands those two references to the two descriptors. */
        g_pipe_read_refs[g_pipe_created] = 1;
        g_pipe_write_refs[g_pipe_created] = 1;
        g_pipe_created++;
        return p;
    }
}

void pipe_ref_read(pipe_t *p) {
    int i = pipe_index(p);
    if (i >= 0) g_pipe_read_refs[i]++;
}

void pipe_ref_write(pipe_t *p) {
    int i = pipe_index(p);
    if (i >= 0) g_pipe_write_refs[i]++;
}

void pipe_close_read(pipe_t *p) {
    int i = pipe_index(p);
    if (i < 0) return;
    if (g_pipe_read_refs[i] == 0) { g_pipe_underflow++; return; }
    g_pipe_read_refs[i]--;
}

void pipe_close_write(pipe_t *p) {
    int i = pipe_index(p);
    if (i < 0) return;
    if (g_pipe_write_refs[i] == 0) { g_pipe_underflow++; return; }
    g_pipe_write_refs[i]--;
}

/* --- the rest of the stub surface ----------------------------------------- */

#include "../process.h"

static process_t *g_current;
process_t *process_get_current(void) { return g_current; }

int paging_user_range_mapped(uint32_t vaddr, uint32_t size) {
    (void)vaddr; (void)size;
    return 1;                       /* the pointer checks are CAP10's subject */
}

#include "../syscall.c"

#include "test.h"

/* --- harness -------------------------------------------------------------- */

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

/* sys_pipe writes the two descriptors through a USER pointer, so its
 * argument has to sit inside [USER_LOAD_BASE, USER_STACK_TOP) or
 * user_buffer_valid refuses the call before any of the ownership logic
 * runs -- the test would then pass for a reason that has nothing to do
 * with what it is checking. Map one real page there. */
static int32_t *g_user_fds;

static int map_user_page(void) {
    void *want = (void *)(uintptr_t)USER_LOAD_BASE;
    void *got = mmap(want, 0x1000, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
                     -1, 0);

    if (got != want) return 0;
    g_user_fds = (int32_t *)got;
    return 1;
}

static process_t g_procs[MAX_PROCESSES];

static void reset_world(void) {
    for (int i = 0; i < MAX_FAKE_NODES; i++) {
        memset(&g_nodes[i], 0, sizeof(g_nodes[i]));
        g_nodes[i].flags = FS_FILE;
        g_nodes[i].inode = (uint32_t)i + 1;
        g_node_refs[i] = 0;
    }
    for (int i = 0; i < MAX_FAKE_PIPES; i++) {
        g_pipe_read_refs[i] = 0;
        g_pipe_write_refs[i] = 0;
    }
    g_node_underflow = 0;
    g_pipe_underflow = 0;
    g_pipe_created = 0;
    g_pipe_create_fails = 0;
    g_current = NULL;

    for (int i = 0; i < MAX_PROCESSES; i++) {
        memset(&g_procs[i], 0, sizeof(g_procs[i]));
        g_procs[i].slot = (uint32_t)i;
        g_procs[i].pid = i + 1;
    }
    /* Start every descriptor table empty, the state process.c relies on for
     * the very first process in each slot. */
    memset(open_files, 0, sizeof(open_files));
}

/* Model a process taking over a slot the way process_allocate does: the
 * process_t is zeroed, and open_files[slot] is NOT touched. */
static process_t *take_slot(uint32_t slot, int32_t pid) {
    process_t *p = &g_procs[slot];

    memset(p, 0, sizeof(*p));
    p->slot = slot;
    p->pid = pid;
    p->state = PROCESS_RUNNING;
    return p;
}

static int table_is_empty(uint32_t slot) {
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (open_files[slot][i].kind != OF_NONE) return 0;
        if (open_files[slot][i].node != NULL) return 0;
        if (open_files[slot][i].pipe != NULL) return 0;
        if (open_files[slot][i].offset != 0) return 0;
    }
    return 1;
}

static int total_node_refs(void) {
    int n = 0;
    for (int i = 0; i < MAX_FAKE_NODES; i++) n += g_node_refs[i];
    return n;
}

static int total_pipe_refs(void) {
    int n = 0;
    for (int i = 0; i < MAX_FAKE_PIPES; i++)
        n += g_pipe_read_refs[i] + g_pipe_write_refs[i];
    return n;
}

static void expect_no_underflow(void) {
    CHECK_EQ(g_node_underflow, 0);
    CHECK_EQ(g_pipe_underflow, 0);
}

/* --- the table's own bookkeeping ------------------------------------------ */

static void test_open_takes_a_reference(void) {
    process_t *p;

    TEST("opening a descriptor takes exactly one reference");
    reset_world();
    p = take_slot(0, 10);
    g_current = p;

    CHECK_EQ(open_user_file(&g_nodes[0]), FIRST_USER_FD);
    CHECK_EQ(g_node_refs[0], 1);

    /* A second descriptor on the same node is a second reference: the node
     * must stay alive until both are closed. */
    CHECK_EQ(open_user_file(&g_nodes[0]), FIRST_USER_FD + 1);
    CHECK_EQ(g_node_refs[0], 2);

    CHECK_EQ(open_user_file(&g_nodes[1]), FIRST_USER_FD + 2);
    CHECK_EQ(g_node_refs[1], 1);
    expect_no_underflow();
}

static void test_close_releases_and_clears(void) {
    process_t *p;

    TEST("close releases the reference AND clears the entry");
    reset_world();
    p = take_slot(0, 10);
    g_current = p;

    CHECK_EQ(open_user_file(&g_nodes[0]), FIRST_USER_FD);
    CHECK_EQ(sys_close(FIRST_USER_FD), 0);
    CHECK_EQ(g_node_refs[0], 0);

    /* Clearing matters as much as releasing: an entry left holding a stale
     * kind and node would be released a second time on exit, and the slot
     * would look occupied to alloc_fd. */
    CHECK_EQ(open_files[0][0].kind, OF_NONE);
    CHECK(open_files[0][0].node == NULL);
    CHECK(open_files[0][0].pipe == NULL);
    CHECK_EQ(open_files[0][0].offset, 0);

    /* Closing again is refused and must not release anything a second time. */
    CHECK_EQ(sys_close(FIRST_USER_FD), -1);
    CHECK_EQ(g_node_refs[0], 0);
    expect_no_underflow();
}

static void test_slot_reuse_after_close(void) {
    process_t *p;

    TEST("a freed descriptor slot is reused");
    reset_world();
    p = take_slot(0, 10);
    g_current = p;

    for (int i = 0; i < MAX_OPEN_FILES; i++)
        CHECK_EQ(open_user_file(&g_nodes[i % MAX_FAKE_NODES]), FIRST_USER_FD + i);

    /* Exhausted: the next request has nowhere to go, and must not take a
     * reference it cannot store. */
    {
        int refs_before = total_node_refs();

        CHECK_EQ(open_user_file(&g_nodes[0]), -1);
        CHECK_EQ(total_node_refs(), refs_before);
    }

    /* Free the middle one and it comes back. */
    CHECK_EQ(sys_close(FIRST_USER_FD + 3), 0);
    CHECK_EQ(open_user_file(&g_nodes[2]), FIRST_USER_FD + 3);
    expect_no_underflow();
}

/* --- the slot-reuse invariant --------------------------------------------- */

static void test_exit_clears_the_table(void) {
    process_t *p;

    TEST("exit closes every descriptor and empties the table");
    reset_world();
    p = take_slot(0, 10);
    g_current = p;

    CHECK_EQ(open_user_file(&g_nodes[0]), FIRST_USER_FD);
    CHECK_EQ(open_user_file(&g_nodes[1]), FIRST_USER_FD + 1);
    CHECK_EQ(open_user_file(&g_nodes[1]), FIRST_USER_FD + 2);
    CHECK_EQ(total_node_refs(), 3);

    syscall_close_user_files(p);

    /* Every reference dropped ... */
    CHECK_EQ(total_node_refs(), 0);
    /* ... and every entry cleared. Dropping the references without clearing
     * would leave the table looking full to the next occupant of this slot,
     * and would release each node a second time when that occupant exits. */
    CHECK(table_is_empty(0));
    expect_no_underflow();
}

static void test_slot_reuse_sees_an_empty_table(void) {
    process_t *a, *b;

    /*
     * The invariant this file exists for. process.c hands slot 0 to a new
     * process without touching open_files[0]; the only thing that empties it
     * is the previous occupant's exit.
     */
    TEST("a reused slot starts with an empty descriptor table");
    reset_world();

    a = take_slot(0, 10);
    g_current = a;
    CHECK_EQ(open_user_file(&g_nodes[0]), FIRST_USER_FD);
    CHECK_EQ(open_user_file(&g_nodes[1]), FIRST_USER_FD + 1);
    syscall_close_user_files(a);              /* process_finish_exit */

    /* A new process takes the same slot, exactly as process_allocate does. */
    b = take_slot(0, 11);
    g_current = b;

    CHECK(table_is_empty(0));
    /* And its first descriptor is fd 3, not whatever the previous occupant
     * left behind. */
    CHECK_EQ(open_user_file(&g_nodes[2]), FIRST_USER_FD);
    CHECK_EQ(g_node_refs[2], 1);
    /* The previous occupant's nodes are untouched by the new process. */
    CHECK_EQ(g_node_refs[0], 0);
    CHECK_EQ(g_node_refs[1], 0);
    expect_no_underflow();
}

static void test_repeated_reuse_accumulates_nothing(void) {
    TEST("repeated allocate/open/exit cycles leak nothing");
    reset_world();

    for (int round = 0; round < 32; round++) {
        uint32_t slot = (uint32_t)(round % MAX_PROCESSES);
        process_t *p = take_slot(slot, 100 + round);

        g_current = p;
        for (int i = 0; i < 4; i++)
            CHECK(open_user_file(&g_nodes[(round + i) % MAX_FAKE_NODES]) >= 0);

        syscall_close_user_files(p);
        CHECK(table_is_empty(slot));
    }

    /* Nothing accumulated over thirty-two cycles: every reference taken was
     * given back, and no object was released more times than it was taken. */
    CHECK_EQ(total_node_refs(), 0);
    expect_no_underflow();
}

static void test_close_user_files_defensive(void) {
    process_t bad;

    TEST("close_user_files: defensive arguments");
    reset_world();

    /* A NULL process, and a slot outside the table, must be no-ops rather
     * than indexing open_files[] out of range. */
    syscall_close_user_files(NULL);

    memset(&bad, 0, sizeof(bad));
    bad.slot = MAX_PROCESSES;              /* one past the end */
    syscall_close_user_files(&bad);
    bad.slot = 0xFFFFFFFFu;
    syscall_close_user_files(&bad);
    expect_no_underflow();

    /* Closing a process that never opened anything is also a no-op. */
    {
        process_t *p = take_slot(1, 20);
        syscall_close_user_files(p);
        CHECK(table_is_empty(1));
    }
}

/* --- fork ----------------------------------------------------------------- */

static void test_fork_copies_and_bumps(void) {
    process_t *parent, *child;

    TEST("fork copies the table and takes a reference for each descriptor");
    reset_world();
    parent = take_slot(0, 10);
    g_current = parent;

    CHECK_EQ(open_user_file(&g_nodes[0]), FIRST_USER_FD);
    CHECK_EQ(open_user_file(&g_nodes[1]), FIRST_USER_FD + 1);
    CHECK_EQ(g_node_refs[0], 1);
    CHECK_EQ(g_node_refs[1], 1);

    child = take_slot(1, 11);
    syscall_copy_user_files(parent, child);

    /* Both descriptors exist twice over, so both nodes are referenced twice.
     * Copying the table without bumping would let the parent's exit free a
     * node the child still has open -- F11's shape. */
    CHECK_EQ(g_node_refs[0], 2);
    CHECK_EQ(g_node_refs[1], 2);

    /* The child's table matches the parent's, including the offsets. */
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        CHECK_EQ(open_files[1][i].kind, open_files[0][i].kind);
        CHECK(open_files[1][i].node == open_files[0][i].node);
        CHECK_EQ(open_files[1][i].offset, open_files[0][i].offset);
    }
    expect_no_underflow();
}

static void test_fork_preserves_offsets(void) {
    process_t *parent, *child;

    TEST("fork carries the file offset across");
    reset_world();
    parent = take_slot(0, 10);
    g_current = parent;

    CHECK_EQ(open_user_file(&g_nodes[0]), FIRST_USER_FD);
    open_files[0][0].offset = 1234;        /* as a read would leave it */

    child = take_slot(1, 11);
    syscall_copy_user_files(parent, child);
    CHECK_EQ(open_files[1][0].offset, 1234);
}

static void test_fork_failure_rollback(void) {
    process_t *parent, *child;

    /*
     * process_fork copies the table into the child and then may still fail --
     * creating the task, or the address space. The rollback path closes the
     * child's files. If it did not, every descriptor the parent held would
     * carry an extra reference for the rest of the machine's life, and the
     * underlying file could never be unlinked.
     */
    TEST("a failed fork gives back everything the child took");
    reset_world();
    parent = take_slot(0, 10);
    g_current = parent;

    CHECK_EQ(open_user_file(&g_nodes[0]), FIRST_USER_FD);
    CHECK_EQ(open_user_file(&g_nodes[1]), FIRST_USER_FD + 1);

    child = take_slot(1, 11);
    syscall_copy_user_files(parent, child);
    CHECK_EQ(total_node_refs(), 4);

    syscall_close_user_files(child);        /* the rollback */

    CHECK_EQ(g_node_refs[0], 1);           /* back to the parent's own */
    CHECK_EQ(g_node_refs[1], 1);
    CHECK(table_is_empty(1));
    /* And the parent is untouched: its descriptors still work. */
    CHECK_EQ(open_files[0][0].kind, OF_FILE);
    CHECK_EQ(sys_close(FIRST_USER_FD), 0);
    CHECK_EQ(g_node_refs[0], 0);
    expect_no_underflow();
}

static void test_child_exit_leaves_parent_usable(void) {
    process_t *parent, *child;

    TEST("a child exiting does not disturb the parent's descriptors");
    reset_world();
    parent = take_slot(0, 10);
    g_current = parent;
    CHECK_EQ(open_user_file(&g_nodes[0]), FIRST_USER_FD);

    child = take_slot(1, 11);
    syscall_copy_user_files(parent, child);
    CHECK_EQ(g_node_refs[0], 2);

    syscall_close_user_files(child);
    CHECK_EQ(g_node_refs[0], 1);           /* the parent's reference survives */
    CHECK(table_is_empty(1));

    g_current = parent;
    CHECK_EQ(open_files[0][0].kind, OF_FILE);
    CHECK_EQ(sys_close(FIRST_USER_FD), 0);
    CHECK_EQ(g_node_refs[0], 0);
    expect_no_underflow();
}

static void test_parent_exit_leaves_child_usable(void) {
    process_t *parent, *child;

    TEST("a parent exiting does not disturb the child's descriptors");
    reset_world();
    parent = take_slot(0, 10);
    g_current = parent;
    CHECK_EQ(open_user_file(&g_nodes[0]), FIRST_USER_FD);

    child = take_slot(1, 11);
    syscall_copy_user_files(parent, child);

    syscall_close_user_files(parent);
    CHECK_EQ(g_node_refs[0], 1);           /* the child's reference survives */
    CHECK(table_is_empty(0));

    g_current = child;
    CHECK_EQ(open_files[1][0].kind, OF_FILE);
    CHECK_EQ(sys_close(FIRST_USER_FD), 0);
    CHECK_EQ(g_node_refs[0], 0);
    expect_no_underflow();
}

static void test_copy_user_files_defensive(void) {
    process_t *parent;
    process_t bad;

    TEST("copy_user_files: defensive arguments");
    reset_world();
    parent = take_slot(0, 10);
    g_current = parent;
    CHECK_EQ(open_user_file(&g_nodes[0]), FIRST_USER_FD);

    memset(&bad, 0, sizeof(bad));
    bad.slot = MAX_PROCESSES;

    syscall_copy_user_files(NULL, &g_procs[1]);
    syscall_copy_user_files(parent, NULL);
    syscall_copy_user_files(parent, &bad);
    syscall_copy_user_files(&bad, parent);

    /* None of those may have taken a reference or written outside the table. */
    CHECK_EQ(g_node_refs[0], 1);
    expect_no_underflow();
}

/* --- pipes ---------------------------------------------------------------- */

static void test_pipe_endpoints_are_separate(void) {
    process_t *p;
    int32_t *fds = g_user_fds;

    TEST("a pipe's two ends are referenced separately");
    reset_world();
    p = take_slot(0, 10);
    g_current = p;

    CHECK_EQ(sys_pipe(fds), 0);
    CHECK_EQ(fds[0], FIRST_USER_FD);
    CHECK_EQ(fds[1], FIRST_USER_FD + 1);
    CHECK_EQ(open_files[0][0].kind, OF_PIPE_R);
    CHECK_EQ(open_files[0][1].kind, OF_PIPE_W);

    /* One reader and one writer, from pipe_create. Counting both ends with a
     * single number would make the reader's close look like it had also shut
     * the writer, and the peer would see EOF while someone was still writing. */
    CHECK_EQ(g_pipe_read_refs[0], 1);
    CHECK_EQ(g_pipe_write_refs[0], 1);

    CHECK_EQ(sys_close(fds[0]), 0);
    CHECK_EQ(g_pipe_read_refs[0], 0);
    CHECK_EQ(g_pipe_write_refs[0], 1);     /* the write end is unaffected */

    CHECK_EQ(sys_close(fds[1]), 0);
    CHECK_EQ(g_pipe_write_refs[0], 0);
    expect_no_underflow();
}

static void test_pipe_survives_fork_and_exit(void) {
    process_t *parent, *child;
    int32_t *fds = g_user_fds;

    TEST("fork duplicates both pipe ends");
    reset_world();
    parent = take_slot(0, 10);
    g_current = parent;
    CHECK_EQ(sys_pipe(fds), 0);

    child = take_slot(1, 11);
    syscall_copy_user_files(parent, child);
    CHECK_EQ(g_pipe_read_refs[0], 2);
    CHECK_EQ(g_pipe_write_refs[0], 2);

    /* The child closing its write end must not close the parent's, or the
     * parent's own reads would see a premature EOF. */
    g_current = child;
    CHECK_EQ(sys_close(fds[1]), 0);
    CHECK_EQ(g_pipe_write_refs[0], 1);

    syscall_close_user_files(child);
    CHECK_EQ(g_pipe_read_refs[0], 1);
    CHECK_EQ(g_pipe_write_refs[0], 1);

    syscall_close_user_files(parent);
    CHECK_EQ(total_pipe_refs(), 0);
    expect_no_underflow();
}

static void test_pipe_creation_failure(void) {
    process_t *p;
    int32_t *fds = g_user_fds;

    TEST("a pipe that cannot be created leaves nothing behind");
    reset_world();
    p = take_slot(0, 10);
    g_current = p;

    g_pipe_create_fails = 1;
    CHECK_EQ(sys_pipe(fds), -1);
    CHECK(table_is_empty(0));
    CHECK_EQ(total_pipe_refs(), 0);

    /* And when the table has no room for the second descriptor, the first one
     * has to be given back along with both pipe ends. */
    g_pipe_create_fails = 0;
    for (int i = 0; i < MAX_OPEN_FILES - 1; i++)
        CHECK(open_user_file(&g_nodes[0]) >= 0);

    CHECK_EQ(sys_pipe(fds), -1);
    CHECK_EQ(total_pipe_refs(), 0);        /* both ends released */
    /* The descriptor the failed call briefly held is free again. */
    CHECK_EQ(open_user_file(&g_nodes[1]), FIRST_USER_FD + MAX_OPEN_FILES - 1);
    expect_no_underflow();
}

/* --- dup2 ----------------------------------------------------------------- */

static void test_dup2_between_table_slots(void) {
    process_t *p;

    TEST("dup2 onto another table slot");
    reset_world();
    p = take_slot(0, 10);
    g_current = p;

    CHECK_EQ(open_user_file(&g_nodes[0]), FIRST_USER_FD);
    CHECK_EQ(open_user_file(&g_nodes[1]), FIRST_USER_FD + 1);
    CHECK_EQ(g_node_refs[0], 1);
    CHECK_EQ(g_node_refs[1], 1);

    /* Overwriting a live descriptor has to release what was there and take a
     * reference for what replaces it. Skipping either half is a leak or a
     * use-after-free, and both leave sys_dup2 returning the same value. */
    CHECK_EQ(sys_dup2(FIRST_USER_FD, FIRST_USER_FD + 1), FIRST_USER_FD + 1);
    CHECK_EQ(g_node_refs[1], 0);           /* the old occupant was released */
    CHECK_EQ(g_node_refs[0], 2);           /* and the source gained one */
    CHECK(open_files[0][1].node == &g_nodes[0]);

    /* Both descriptors now name the same node; closing one leaves the other. */
    CHECK_EQ(sys_close(FIRST_USER_FD), 0);
    CHECK_EQ(g_node_refs[0], 1);
    CHECK_EQ(sys_close(FIRST_USER_FD + 1), 0);
    CHECK_EQ(g_node_refs[0], 0);
    expect_no_underflow();
}

static void test_dup2_onto_itself(void) {
    process_t *p;

    TEST("dup2 onto itself changes nothing");
    reset_world();
    p = take_slot(0, 10);
    g_current = p;
    CHECK_EQ(open_user_file(&g_nodes[0]), FIRST_USER_FD);

    /* The obvious implementation -- close the target, then copy -- would
     * release the only reference and then hand back a descriptor pointing at
     * a node it no longer owns. */
    CHECK_EQ(sys_dup2(FIRST_USER_FD, FIRST_USER_FD), FIRST_USER_FD);
    CHECK_EQ(g_node_refs[0], 1);
    CHECK_EQ(open_files[0][0].kind, OF_FILE);
    CHECK(open_files[0][0].node == &g_nodes[0]);
    expect_no_underflow();
}

static void test_dup2_onto_stdout(void) {
    process_t *p;

    TEST("dup2 onto stdout takes its own reference");
    reset_world();
    p = take_slot(0, 10);
    g_current = p;
    CHECK_EQ(open_user_file(&g_nodes[0]), FIRST_USER_FD);

    CHECK_EQ(sys_dup2(FIRST_USER_FD, 1), 1);
    CHECK(p->stdout_node == &g_nodes[0]);
    /* Two references: the descriptor and the stdout slot. The dup2-then-close
     * idiom depends on this -- without the second reference the node would be
     * unreferenced the moment the descriptor closed, and the next write would
     * go through a freed node (F11). */
    CHECK_EQ(g_node_refs[0], 2);

    CHECK_EQ(sys_close(FIRST_USER_FD), 0);
    CHECK_EQ(g_node_refs[0], 1);           /* stdout still holds one */

    /* Replacing stdout releases the previous target. */
    CHECK_EQ(open_user_file(&g_nodes[1]), FIRST_USER_FD);
    CHECK_EQ(sys_dup2(FIRST_USER_FD, 1), 1);
    CHECK_EQ(g_node_refs[0], 0);
    CHECK_EQ(g_node_refs[1], 2);
    expect_no_underflow();
}

static void test_dup2_rejected_pipe_end_preserves_target(void) {
    process_t *p;
    int32_t *fds = g_user_fds;

    TEST("dup2 rejection leaves stdin and stdout unchanged");
    reset_world();
    p = take_slot(0, 10);
    g_current = p;

    CHECK_EQ(sys_pipe(fds), 0);
    CHECK_EQ(sys_dup2(fds[1], 1), 1);
    CHECK(p->stdout_pipe == &g_pipes[0]);
    CHECK_EQ(g_pipe_write_refs[0], 2);

    /* A read end cannot become stdout.  Rejecting it must not close the
     * descriptor already installed there. */
    CHECK_EQ(sys_dup2(fds[0], 1), -1);
    CHECK(p->stdout_pipe == &g_pipes[0]);
    CHECK_EQ(g_pipe_write_refs[0], 2);

    CHECK_EQ(sys_dup2(fds[0], 0), 0);
    CHECK(p->stdin_pipe == &g_pipes[0]);
    CHECK_EQ(g_pipe_read_refs[0], 2);

    /* The inverse mismatch has the same replace-on-success requirement. */
    CHECK_EQ(sys_dup2(fds[1], 0), -1);
    CHECK(p->stdin_pipe == &g_pipes[0]);
    CHECK_EQ(g_pipe_read_refs[0], 2);
    expect_no_underflow();
}

static void test_dup2_defensive(void) {
    process_t *p;

    TEST("dup2: invalid descriptors");
    reset_world();
    p = take_slot(0, 10);
    g_current = p;
    CHECK_EQ(open_user_file(&g_nodes[0]), FIRST_USER_FD);

    CHECK_EQ(sys_dup2(0, 1), -1);                          /* stdin as source */
    CHECK_EQ(sys_dup2(FIRST_USER_FD - 1, FIRST_USER_FD), -1);
    CHECK_EQ(sys_dup2(FIRST_USER_FD + MAX_OPEN_FILES, 1), -1);
    CHECK_EQ(sys_dup2(FIRST_USER_FD + 1, 1), -1);          /* source not open */
    CHECK_EQ(sys_dup2(FIRST_USER_FD, 2), -1);              /* stderr: no slot */
    CHECK_EQ(sys_dup2(FIRST_USER_FD,
                      FIRST_USER_FD + MAX_OPEN_FILES), -1);

    /* None of those may have moved a reference. */
    CHECK_EQ(g_node_refs[0], 1);
    expect_no_underflow();
}

/* --- the two kinds must not be confused ----------------------------------- */

static void test_kinds_are_not_confused(void) {
    process_t *p;
    int32_t *fds = g_user_fds;

    /*
     * A file entry carries a node, a pipe entry carries a pipe, and the release
     * path picks by kind. Releasing a file as a pipe end (or the read end as a
     * write end) would decrement the wrong counter -- which no return value
     * reflects, and which the shell could not tell apart from working.
     */
    TEST("file and pipe descriptors release their own kind");
    reset_world();
    p = take_slot(0, 10);
    g_current = p;

    CHECK_EQ(open_user_file(&g_nodes[0]), FIRST_USER_FD);
    CHECK_EQ(sys_pipe(fds), 0);
    CHECK_EQ(g_node_refs[0], 1);
    CHECK_EQ(g_pipe_read_refs[0], 1);
    CHECK_EQ(g_pipe_write_refs[0], 1);

    syscall_close_user_files(p);

    CHECK_EQ(g_node_refs[0], 0);
    CHECK_EQ(g_pipe_read_refs[0], 0);
    CHECK_EQ(g_pipe_write_refs[0], 0);
    expect_no_underflow();                 /* nothing released twice */
}

static void test_mixed_table_across_fork_and_exit(void) {
    process_t *parent, *child;
    int32_t *fds = g_user_fds;

    TEST("a mixed table survives fork and two exits");
    reset_world();
    parent = take_slot(0, 10);
    g_current = parent;

    CHECK_EQ(open_user_file(&g_nodes[0]), FIRST_USER_FD);
    CHECK_EQ(sys_pipe(fds), 0);
    CHECK_EQ(open_user_file(&g_nodes[1]), FIRST_USER_FD + 3);

    child = take_slot(1, 11);
    syscall_copy_user_files(parent, child);
    CHECK_EQ(g_node_refs[0], 2);
    CHECK_EQ(g_node_refs[1], 2);
    CHECK_EQ(g_pipe_read_refs[0], 2);
    CHECK_EQ(g_pipe_write_refs[0], 2);

    syscall_close_user_files(child);
    syscall_close_user_files(parent);

    CHECK_EQ(total_node_refs(), 0);
    CHECK_EQ(total_pipe_refs(), 0);
    CHECK(table_is_empty(0));
    CHECK(table_is_empty(1));
    expect_no_underflow();

    /* Both slots are now safe to hand to new processes. */
    {
        process_t *c = take_slot(0, 12);
        g_current = c;
        CHECK_EQ(open_user_file(&g_nodes[2]), FIRST_USER_FD);
        CHECK_EQ(g_node_refs[2], 1);
    }
}

static void test_no_current_process(void) {
    TEST("descriptor calls with no current process");
    reset_world();
    g_current = NULL;

    /* Before the first process exists, and in kernel-shell context, there is
     * no table. Every entry point must refuse rather than index open_files[]
     * through a NULL process. */
    CHECK_EQ(open_user_file(&g_nodes[0]), -1);
    CHECK_EQ(sys_close(FIRST_USER_FD), -1);
    CHECK_EQ(sys_dup2(FIRST_USER_FD, 1), -1);
    CHECK_EQ(g_node_refs[0], 0);
    expect_no_underflow();

    /* A process whose slot is out of range is treated the same way. */
    {
        process_t bad;

        memset(&bad, 0, sizeof(bad));
        bad.slot = MAX_PROCESSES;
        g_current = &bad;
        CHECK_EQ(open_user_file(&g_nodes[0]), -1);
        CHECK_EQ(sys_close(FIRST_USER_FD), -1);
        CHECK_EQ(g_node_refs[0], 0);
    }
    expect_no_underflow();
}

static void test_exit_closes_every_slot(void) {
    process_t *p;

    /*
     * Every descriptor, including the last one in the table. The other exit
     * tests open two or three, so a loop that stops one short -- or starts one
     * late -- closes everything they check and leaves no trace. Filling the
     * table is what makes the bounds of that loop observable.
     */
    TEST("exit closes the first and last descriptor too");
    reset_world();
    p = take_slot(0, 10);
    g_current = p;

    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        CHECK_EQ(open_user_file(&g_nodes[i % MAX_FAKE_NODES]), FIRST_USER_FD + i);
    }
    CHECK_EQ(total_node_refs(), MAX_OPEN_FILES);

    syscall_close_user_files(p);

    CHECK_EQ(total_node_refs(), 0);
    CHECK(table_is_empty(0));
    expect_no_underflow();
}

static void test_fork_copies_every_slot(void) {
    process_t *parent, *child;

    /* Same reasoning for the copy: a loop that skips the last entry leaves the
     * child without a descriptor the parent had, and leaves that object with
     * one reference too few for the number of tables pointing at it. */
    TEST("fork copies the first and last descriptor too");
    reset_world();
    parent = take_slot(0, 10);
    g_current = parent;

    for (int i = 0; i < MAX_OPEN_FILES; i++)
        CHECK_EQ(open_user_file(&g_nodes[i % MAX_FAKE_NODES]), FIRST_USER_FD + i);

    child = take_slot(1, 11);
    syscall_copy_user_files(parent, child);

    /* Every slot present in both tables, and every object referenced twice. */
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        CHECK_EQ(open_files[1][i].kind, OF_FILE);
        CHECK(open_files[1][i].node == open_files[0][i].node);
    }
    CHECK_EQ(total_node_refs(), 2 * MAX_OPEN_FILES);

    syscall_close_user_files(child);
    CHECK_EQ(total_node_refs(), MAX_OPEN_FILES);
    syscall_close_user_files(parent);
    CHECK_EQ(total_node_refs(), 0);
    expect_no_underflow();
}

static void test_descriptor_slot_starts_clean(void) {
    process_t *p;

    /*
     * A descriptor slot is reused within a single process's lifetime too, and
     * it must come back as new. The offset is the part with teeth: leave the
     * previous owner's value behind and a freshly opened file starts reading
     * from wherever the last one stopped, which no return value reveals and
     * which looks like file corruption to the program.
     */
    TEST("a recycled descriptor slot starts at offset zero");
    reset_world();
    p = take_slot(0, 10);
    g_current = p;

    CHECK_EQ(open_user_file(&g_nodes[0]), FIRST_USER_FD);
    open_files[0][0].offset = 4096;            /* as reads would leave it */
    CHECK_EQ(sys_close(FIRST_USER_FD), 0);

    CHECK_EQ(open_user_file(&g_nodes[1]), FIRST_USER_FD);
    CHECK_EQ(open_files[0][0].offset, 0);
    CHECK(open_files[0][0].node == &g_nodes[1]);
    CHECK(open_files[0][0].pipe == NULL);
    expect_no_underflow();
}

static void test_pipe_ends_bumped_by_fork_are_not_swapped(void) {
    process_t *parent, *child;
    int32_t *fds = g_user_fds;

    /*
     * With both ends open, a fork that bumped the wrong counter is invisible:
     * both go from one to two either way. Closing the write end first makes
     * the two counts differ, so swapping them shows up.
     */
    TEST("fork bumps the pipe end the descriptor actually holds");
    reset_world();
    parent = take_slot(0, 10);
    g_current = parent;

    CHECK_EQ(sys_pipe(fds), 0);
    CHECK_EQ(sys_close(fds[1]), 0);            /* keep only the read end */
    CHECK_EQ(g_pipe_read_refs[0], 1);
    CHECK_EQ(g_pipe_write_refs[0], 0);

    child = take_slot(1, 11);
    syscall_copy_user_files(parent, child);

    CHECK_EQ(g_pipe_read_refs[0], 2);          /* the read end gained one */
    CHECK_EQ(g_pipe_write_refs[0], 0);         /* the write end is gone */
    expect_no_underflow();

    syscall_close_user_files(child);
    syscall_close_user_files(parent);
    CHECK_EQ(total_pipe_refs(), 0);
    expect_no_underflow();
}

static void test_pipe_with_no_free_descriptor(void) {
    process_t *p;
    int32_t *fds = g_user_fds;

    /*
     * A full table, so the FIRST allocation fails. The other rollback test
     * leaves one slot free and therefore only exercises the second failure;
     * the two paths release different things, and the earlier one has to give
     * back both pipe ends because it never handed either to a descriptor.
     */
    TEST("a pipe with no free descriptor at all releases both ends");
    reset_world();
    p = take_slot(0, 10);
    g_current = p;

    for (int i = 0; i < MAX_OPEN_FILES; i++)
        CHECK(open_user_file(&g_nodes[i % MAX_FAKE_NODES]) >= 0);

    CHECK_EQ(sys_pipe(fds), -1);
    CHECK_EQ(total_pipe_refs(), 0);            /* nothing left holding it */
    CHECK_EQ(total_node_refs(), MAX_OPEN_FILES);   /* files untouched */
    expect_no_underflow();
}

int main(void) {
    int have_user_page = map_user_page();

    test_open_takes_a_reference();
    test_close_releases_and_clears();
    test_slot_reuse_after_close();

    test_exit_clears_the_table();
    test_exit_closes_every_slot();
    test_descriptor_slot_starts_clean();
    test_slot_reuse_sees_an_empty_table();
    test_repeated_reuse_accumulates_nothing();
    test_close_user_files_defensive();

    test_fork_copies_and_bumps();
    test_fork_copies_every_slot();
    test_fork_preserves_offsets();
    test_fork_failure_rollback();
    test_child_exit_leaves_parent_usable();
    test_parent_exit_leaves_child_usable();
    test_copy_user_files_defensive();

    if (have_user_page) {
        test_pipe_endpoints_are_separate();
        test_pipe_survives_fork_and_exit();
        test_pipe_creation_failure();
    } else {
        printf("  SKIP pipe tests: could not map a page at USER_LOAD_BASE\n");
    }

    test_dup2_between_table_slots();
    test_dup2_onto_itself();
    test_dup2_onto_stdout();
    if (have_user_page)
        test_dup2_rejected_pipe_end_preserves_target();
    test_dup2_defensive();

    if (have_user_page) {
        test_kinds_are_not_confused();
        test_mixed_table_across_fork_and_exit();
        test_pipe_ends_bumped_by_fork_are_not_swapped();
        test_pipe_with_no_free_descriptor();
    }
    test_no_current_process();

    TEST_REPORT("fdtable");
}
