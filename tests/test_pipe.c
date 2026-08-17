#include "test.h"
#include "../pipe.h"
#include "../task.h"

#include <setjmp.h>
#include <stdlib.h>
#include <string.h>

/*
 * The pipe is a blocking ring buffer, and both properties are hard to test
 * from the shell: the wrap-around indexing only shows up once read_pos or
 * write_pos crosses PIPE_BUF_SIZE (the shell moves a few bytes at a time), and
 * the blocking transitions cannot be single-stepped there at all.
 *
 * task_block_current is replaced with a scripted hook that stands in for the
 * peer task: when the code blocks, the hook mutates the pipe exactly as a
 * running reader/writer/closer would, so the blocking loop's exit conditions
 * (data arrived, EOF, space freed, broken pipe) are exercised deterministically
 * on a single thread. With no hook installed a block is treated as a failure
 * and the loops are forced to terminate, so an unexpected block is caught
 * rather than hanging.
 */

/* --- stubs for the scheduler / allocator the pipe pulls in ---------------- */
static pipe_t *g_pipe;                       /* pipe under test, for the escape hatch */
static void  (*g_block_hook)(const void *);  /* scripted peer action */
static int     g_block_count;
static int     g_kfrees;
static int     g_inside_block_hook;
static int     g_defer_free;
static int     g_free_during_block;
static void   *g_deferred_free;
static int     g_kill_pending;
static int     g_expect_exit;
static int32_t g_exit_status;
static jmp_buf g_exit_jmp;
static int     g_peer_write_result;

void task_block_current(const void *chan) {
    g_block_count++;
    if (g_block_hook) {
        g_inside_block_hook = 1;
        g_block_hook(chan);
        g_inside_block_hook = 0;
        return;
    }
    /* Unexpected block: force every pipe loop to terminate so the test fails
     * cleanly instead of spinning. */
    if (g_pipe) { g_pipe->readers = 0; g_pipe->writers = 0; g_pipe->count = 0; }
}
void task_wake_one(const void *chan) { (void)chan; }
void task_wake_all(const void *chan) { (void)chan; }

int task_kill_pending(void) { return g_kill_pending; }
void task_exit(int32_t status) {
    if (g_expect_exit) {
        g_exit_status = status;
        longjmp(g_exit_jmp, 1);
    }
    printf("  FAIL unexpected task_exit(%d)\n", status);
    exit(1);
}

void *kmalloc(size_t n) { return malloc(n ? n : 1); }
void kfree(void *p) {
    g_kfrees++;
    if (g_inside_block_hook) g_free_during_block++;
    if (g_defer_free) {
        g_deferred_free = p;
        return;
    }
    free(p);
}

static void reset_hooks(pipe_t *p) {
    g_pipe = p;
    g_block_hook = NULL;
    g_block_count = 0;
    g_inside_block_hook = 0;
    g_free_during_block = 0;
    g_kill_pending = 0;
}

static void release_deferred_free(void) {
    free(g_deferred_free);
    g_deferred_free = NULL;
    g_defer_free = 0;
}

/* --- non-blocking behaviour ----------------------------------------------- */

static void test_basic(void) {
    pipe_t *p = pipe_create();
    uint8_t buf[16];

    TEST("create");
    CHECK(p != NULL);
    if (!p) return;
    reset_hooks(p);

    TEST("write then read");
    CHECK_EQ(pipe_write(p, (const uint8_t *)"hello", 5), 5);
    CHECK_EQ(pipe_read(p, buf, 5), 5);
    CHECK_EQ(memcmp(buf, "hello", 5), 0);
    CHECK_EQ(g_block_count, 0);           /* nothing above should have blocked */

    TEST("partial read");
    CHECK_EQ(pipe_write(p, (const uint8_t *)"abcdefghij", 10), 10);
    CHECK_EQ(pipe_read(p, buf, 4), 4);    /* asked for less than available */
    CHECK_EQ(memcmp(buf, "abcd", 4), 0);
    CHECK_EQ(pipe_read(p, buf, 100), 6);  /* asked for more than available */
    CHECK_EQ(memcmp(buf, "efghij", 6), 0);

    TEST("zero-length / NULL args");
    CHECK_EQ(pipe_write(p, (const uint8_t *)"x", 0), 0);
    CHECK_EQ(pipe_read(p, buf, 0), 0);
    CHECK_EQ(pipe_write(p, NULL, 5), 0);
    CHECK_EQ(pipe_read(p, NULL, 5), 0);
    CHECK_EQ(g_block_count, 0);

    pipe_close_read(p);
    pipe_close_write(p);
}

static void test_ring_wraparound(void) {
    pipe_t *p = pipe_create();
    static uint8_t out[3000], in[3000];
    unsigned i, round;

    /* Push 3000 bytes through repeatedly. Each pass advances the positions by
     * 3000, so after the second pass write_pos and read_pos have both wrapped
     * past PIPE_BUF_SIZE (4096) -- an off-by-one in the modulo would corrupt a
     * byte at the seam. count never reaches full and never hits empty while a
     * writer is live, so nothing blocks. */
    TEST("ring buffer wrap-around");
    CHECK(p != NULL);
    if (!p) return;
    reset_hooks(p);

    for (round = 0; round < 8; round++) {
        for (i = 0; i < sizeof(out); i++)
            out[i] = (uint8_t)((i * 37 + round * 101) & 0xFF);
        CHECK_EQ(pipe_write(p, out, sizeof(out)), sizeof(out));
        memset(in, 0, sizeof(in));
        CHECK_EQ(pipe_read(p, in, sizeof(in)), sizeof(in));
        CHECK_EQ(memcmp(in, out, sizeof(out)), 0);
    }
    CHECK_EQ(g_block_count, 0);

    pipe_close_read(p);
    pipe_close_write(p);
}

static void test_eof(void) {
    pipe_t *p = pipe_create();
    uint8_t buf[16];

    /* A read on an empty pipe whose writers have all closed is EOF, and must
     * return 0 without blocking. */
    TEST("EOF after writer closes");
    CHECK(p != NULL);
    if (!p) return;
    reset_hooks(p);

    CHECK_EQ(pipe_write(p, (const uint8_t *)"tail", 4), 4);
    pipe_close_write(p);                  /* writers now 0 */

    CHECK_EQ(pipe_read(p, buf, 2), 2);    /* residual data still drains */
    CHECK_EQ(memcmp(buf, "ta", 2), 0);
    CHECK_EQ(pipe_read(p, buf, 16), 2);   /* the rest */
    CHECK_EQ(memcmp(buf, "il", 2), 0);
    CHECK_EQ(pipe_read(p, buf, 16), 0);   /* now empty + no writers -> EOF */
    CHECK_EQ(g_block_count, 0);           /* EOF must not have blocked */

    pipe_close_read(p);
}

static void test_broken_pipe(void) {
    pipe_t *p = pipe_create();

    TEST("broken pipe: no readers");
    CHECK(p != NULL);
    if (!p) return;
    reset_hooks(p);

    pipe_close_read(p);                   /* readers now 0 */
    CHECK_EQ(pipe_write(p, (const uint8_t *)"data", 4), 0);   /* nobody to read */
    CHECK_EQ(g_block_count, 0);           /* must not block on a broken pipe */

    pipe_close_write(p);
}

/* --- reference counting and freeing --------------------------------------- */

static void test_refcount_free(void) {
    pipe_t *p = pipe_create();

    TEST("free only when both ends closed");
    CHECK(p != NULL);
    if (!p) return;
    reset_hooks(p);
    g_kfrees = 0;

    pipe_ref_read(p);                     /* readers: 1 -> 2 */
    pipe_ref_write(p);                    /* writers: 1 -> 2 */

    pipe_close_read(p);                   /* readers 1 */
    pipe_close_write(p);                  /* writers 1 */
    CHECK_EQ(g_kfrees, 0);                /* still referenced, not freed */

    pipe_close_write(p);                  /* writers 0 */
    CHECK_EQ(g_kfrees, 0);                /* reader still holds it */
    pipe_close_read(p);                   /* readers 0 -> freed exactly here */
    CHECK_EQ(g_kfrees, 1);
    /* p is now dangling; do not touch it. */
}

/* --- blocking transitions, driven by the scripted peer -------------------- */

static int g_inject_len;

/* Peer action: a writer appends g_inject_len bytes ('Z') so a blocked reader
 * finds data. */
static void hook_writer_provides(const void *chan) {
    (void)chan;
    uint8_t z[8];
    for (int i = 0; i < g_inject_len; i++) z[i] = 'Z';
    /* Write directly into the ring, mimicking a writer task. */
    for (int i = 0; i < g_inject_len; i++) {
        g_pipe->buf[g_pipe->write_pos] = z[i];
        g_pipe->write_pos = (g_pipe->write_pos + 1) % PIPE_BUF_SIZE;
        g_pipe->count++;
    }
}

/* Peer action: the last writer closes, so a blocked reader sees EOF. */
static void hook_writer_closes(const void *chan) {
    (void)chan;
    g_pipe->writers = 0;
}

/* Peer action: a reader drains everything, freeing space for a blocked writer. */
static void hook_reader_drains(const void *chan) {
    (void)chan;
    g_pipe->count = 0;
    g_pipe->read_pos = g_pipe->write_pos;
}

/* Peer action: another thread closes the process's last descriptors while the
 * current syscall is asleep. The operation itself must keep the pipe alive
 * until it resumes and stops touching the object. */
static void hook_closer_drops_both_ends(const void *chan) {
    (void)chan;
    pipe_close_read(g_pipe);
    pipe_close_write(g_pipe);
}

static void hook_closer_kills_and_drops_both_ends(const void *chan) {
    hook_closer_drops_both_ends(chan);
    g_kill_pending = 1;
}

/* Closing the descriptor must not make an already-blocked read disappear as a
 * reader. A live writer should still be able to satisfy that syscall. */
static void hook_reader_fd_closes_then_writer_sends(const void *chan) {
    (void)chan;
    pipe_close_read(g_pipe);
    g_peer_write_result = (int)pipe_write(
        g_pipe, (const uint8_t *)"Q", 1);
    if (g_peer_write_result == 0) pipe_close_write(g_pipe);
}

static void test_read_blocks_then_data(void) {
    pipe_t *p = pipe_create();
    uint8_t buf[16];

    TEST("read blocks, then a writer provides data");
    CHECK(p != NULL);
    if (!p) return;
    reset_hooks(p);
    g_block_hook = hook_writer_provides;
    g_inject_len = 3;

    /* Empty pipe, one writer alive -> the read blocks once, the hook injects
     * 3 bytes, and the read then completes with them. */
    CHECK_EQ(pipe_read(p, buf, 3), 3);
    CHECK_EQ(g_block_count, 1);
    CHECK_EQ(buf[0], 'Z');

    pipe_close_read(p);
    pipe_close_write(p);
}

static void test_read_blocks_then_eof(void) {
    pipe_t *p = pipe_create();
    uint8_t buf[16];

    TEST("read blocks, then the writer closes -> EOF");
    CHECK(p != NULL);
    if (!p) return;
    reset_hooks(p);
    g_block_hook = hook_writer_closes;

    CHECK_EQ(pipe_read(p, buf, 4), 0);    /* woken by close, sees EOF */
    CHECK_EQ(g_block_count, 1);

    pipe_close_read(p);
}

static void test_write_blocks_then_space(void) {
    pipe_t *p = pipe_create();
    static uint8_t big[PIPE_BUF_SIZE];
    unsigned i;

    TEST("write blocks on full, then a reader drains");
    CHECK(p != NULL);
    if (!p) return;
    reset_hooks(p);

    /* Fill the pipe exactly. */
    for (i = 0; i < sizeof(big); i++) big[i] = (uint8_t)i;
    CHECK_EQ(pipe_write(p, big, sizeof(big)), sizeof(big));
    CHECK_EQ(g_block_count, 0);           /* filling exactly must not block */

    /* One more byte: the pipe is full, so the write blocks; the hook drains it,
     * and the write then completes. */
    g_block_hook = hook_reader_drains;
    CHECK_EQ(pipe_write(p, (const uint8_t *)"!", 1), 1);
    CHECK_EQ(g_block_count, 1);

    pipe_close_read(p);
    pipe_close_write(p);
}

static void test_blocked_operations_hold_lifetime(void) {
    pipe_t *p = pipe_create();
    uint8_t byte;
    static uint8_t full[PIPE_BUF_SIZE];

    TEST("blocked read survives concurrent last close");
    CHECK(p != NULL);
    if (!p) return;
    reset_hooks(p);
    g_kfrees = 0;
    g_defer_free = 1;
    g_deferred_free = NULL;
    g_block_hook = hook_closer_drops_both_ends;

    CHECK_EQ(pipe_read(p, &byte, 1), 0);
    CHECK_EQ(g_block_count, 1);
    CHECK_EQ(g_free_during_block, 0);
    CHECK_EQ(g_kfrees, 1);
    release_deferred_free();

    TEST("blocked write survives concurrent last close");
    p = pipe_create();
    CHECK(p != NULL);
    if (!p) return;
    reset_hooks(p);
    memset(full, 'X', sizeof(full));
    CHECK_EQ(pipe_write(p, full, sizeof(full)), sizeof(full));
    g_kfrees = 0;
    g_defer_free = 1;
    g_deferred_free = NULL;
    g_block_hook = hook_closer_drops_both_ends;

    CHECK_EQ(pipe_write(p, (const uint8_t *)"!", 1), 0);
    CHECK_EQ(g_block_count, 1);
    CHECK_EQ(g_free_during_block, 0);
    CHECK_EQ(g_kfrees, 1);
    release_deferred_free();
}

static void test_killed_operation_releases_lifetime(void) {
    pipe_t *p = pipe_create();
    uint8_t byte;

    TEST("killed blocked read releases its operation lifetime");
    CHECK(p != NULL);
    if (!p) return;
    reset_hooks(p);
    g_kfrees = 0;
    g_defer_free = 1;
    g_deferred_free = NULL;
    g_block_hook = hook_closer_kills_and_drops_both_ends;
    g_expect_exit = 1;

    if (setjmp(g_exit_jmp) == 0) {
        (void)pipe_read(p, &byte, 1);
        CHECK(0);  /* a pending kill must not return to the caller */
    } else {
        CHECK_EQ(g_exit_status, TASK_KILL_STATUS);
        CHECK_EQ(g_free_during_block, 0);
        CHECK_EQ(g_kfrees, 1);
    }

    g_expect_exit = 0;
    release_deferred_free();
}

static void test_blocked_read_remains_a_reader(void) {
    pipe_t *p = pipe_create();
    uint8_t byte = 0;

    TEST("in-flight read remains live after descriptor close");
    CHECK(p != NULL);
    if (!p) return;
    reset_hooks(p);
    g_kfrees = 0;
    g_defer_free = 1;
    g_deferred_free = NULL;
    g_peer_write_result = -1;
    g_block_hook = hook_reader_fd_closes_then_writer_sends;

    CHECK_EQ(pipe_read(p, &byte, 1), 1);
    CHECK_EQ(g_peer_write_result, 1);
    CHECK_EQ(byte, 'Q');
    CHECK_EQ(g_block_count, 1);

    /* The hook closed the descriptor's read reference. Only the real write
     * descriptor remains after pipe_read drops its in-flight reference. */
    if (g_peer_write_result == 1) pipe_close_write(p);
    CHECK_EQ(g_kfrees, 1);
    release_deferred_free();
}

int main(void) {
    test_basic();
    test_ring_wraparound();
    test_eof();
    test_broken_pipe();
    test_refcount_free();
    test_read_blocks_then_data();
    test_read_blocks_then_eof();
    test_write_blocks_then_space();
    test_blocked_operations_hold_lifetime();
    test_killed_operation_releases_lifetime();
    test_blocked_read_remains_a_reader();
    TEST_REPORT("pipe");
}
