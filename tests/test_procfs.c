#include "test.h"

/*
 * procfs: the synthetic /proc filesystem. Five generators render kernel state
 * into one shared 512-byte buffer, and every read regenerates from scratch.
 *
 * Why this module, and why now:
 *
 *   - F3 was a buffer overflow here, fixed by a bound check in the
 *     /proc/processes generator. That check has never once executed. It only
 *     fires when the lines get long, and lines only get long when pids reach
 *     ten digits (next_pid never resets) or process names fill their field --
 *     neither of which happens in a QEMU run that boots, types, and exits.
 *     A fix whose only guard has never run is a fix nobody has tested.
 *
 *   - Two of the five generators have NO bound check at all. They are safe
 *     today purely because their fields happen to be narrow enough
 *     (/proc/self/status works out to 75 bytes at worst, against a 512-byte
 *     buffer). That is a fact about the current field widths, not a property
 *     anything enforces -- widen PROCESS_NAME_MAX and nothing complains. The
 *     tests below are the thing that complains.
 *
 * The buffer invariant is checked structurally rather than by eye: gen_buf is
 * poisoned before every generate, and afterwards every byte from the reported
 * length to the end of the buffer must still be poison. That proves the
 * generator wrote exactly the bytes it claims and not one more -- an
 * assertion on the return value alone would miss a write past the end.
 * -fsanitize=bounds is on as a second, independent net.
 */

#include "../process.h"
#include "../timer.h"

/* --- stubs ---------------------------------------------------------------- */

/* The snapshot the process table would hand back. */
static process_info_t g_snapshot[MAX_PROCESSES];
static uint32_t       g_snapshot_count;
static int            g_snapshot_calls;

uint32_t process_snapshot(process_info_t *out, uint32_t max) {
    uint32_t n = g_snapshot_count < max ? g_snapshot_count : max;

    g_snapshot_calls++;
    if (!out) return 0;
    for (uint32_t i = 0; i < n; i++) out[i] = g_snapshot[i];
    return n;
}

static process_t *g_current;
process_t *process_get_current(void) { return g_current; }

static uint32_t g_ticks;
uint32_t timer_get_ticks(void) { return g_ticks; }

#include "../procfs.c"

/* --- helpers -------------------------------------------------------------- */

#define POISON 0x7F

static void poison_buffer(void) {
    for (unsigned i = 0; i < sizeof(gen_buf); i++) gen_buf[i] = (char)POISON;
}

/* Every byte from `len` to the end of the buffer must be untouched. Together
 * with len <= sizeof(gen_buf) this is the whole no-overflow property: the
 * generator wrote [0, len) and nothing else. */
static int tail_untouched(uint32_t len) {
    if (len > sizeof(gen_buf)) return 0;
    for (unsigned i = len; i < sizeof(gen_buf); i++) {
        if (gen_buf[i] != (char)POISON) return 0;
    }
    return 1;
}

/* Run a generator with the buffer poisoned and return what it produced, as a
 * NUL-terminated copy so the tests can compare strings. */
static char g_out[sizeof(gen_buf) + 1];

static uint32_t generate(uint32_t which) {
    uint32_t len;

    poison_buffer();
    len = proc_generate(which);

    CHECK(len <= sizeof(gen_buf));
    CHECK(tail_untouched(len));

    if (len > sizeof(gen_buf)) len = sizeof(gen_buf);
    for (uint32_t i = 0; i < len; i++) g_out[i] = gen_buf[i];
    g_out[len] = '\0';
    return len;
}

static void set_snapshot(uint32_t count) {
    g_snapshot_count = count;
}

static void add_process(uint32_t index, int32_t pid, int32_t ppid,
                        process_state_t state, const char *name) {
    int i = 0;

    g_snapshot[index].pid = pid;
    g_snapshot[index].parent_pid = ppid;
    g_snapshot[index].state = state;
    while (i < PROCESS_NAME_MAX - 1 && name[i]) {
        g_snapshot[index].name[i] = name[i];
        i++;
    }
    g_snapshot[index].name[i] = '\0';
}

static process_t g_process_storage;

static process_t *make_process(int32_t pid, int32_t ppid, process_state_t state,
                               const char *name, uint32_t cpu_ticks) {
    int i = 0;

    memset(&g_process_storage, 0, sizeof(g_process_storage));
    g_process_storage.pid = pid;
    g_process_storage.parent_pid = ppid;
    g_process_storage.state = state;
    g_process_storage.cpu_ticks = cpu_ticks;
    while (i < PROCESS_NAME_MAX - 1 && name[i]) {
        g_process_storage.name[i] = name[i];
        i++;
    }
    g_process_storage.name[i] = '\0';
    return &g_process_storage;
}

static void reset_all(void) {
    g_snapshot_count = 0;
    g_snapshot_calls = 0;
    g_current = NULL;
    g_ticks = 0;
    for (int i = 0; i < MAX_PROCESSES; i++)
        add_process((uint32_t)i, 0, 0, PROCESS_RUNNING, "");
}

/* --- directory shape ------------------------------------------------------ */

static void test_root_shape(void) {
    fs_node_t *root;

    TEST("root node shape");
    reset_all();
    root = procfs_get_root();

    CHECK(root != NULL);
    CHECK_EQ(root->flags, FS_DIRECTORY);
    CHECK(root->readdir != NULL);
    CHECK(root->finddir != NULL);

    /* No mutating operations at all. /proc is synthesised, so create/unlink/
     * mkdir/rmdir must be absent rather than present-and-failing: the VFS
     * checks the pointer before calling it, and a NULL here is what makes
     * "rm /proc/uptime" a clean error instead of a jump through NULL in
     * ring 0 (tests/test_fs.c pins the VFS half of this). */
    CHECK(root->create == NULL);
    CHECK(root->unlink == NULL);
    CHECK(root->mkdir == NULL);
    CHECK(root->rmdir == NULL);

    /* Nor any way to write to a file: /proc is read-only by construction. */
    for (int i = 0; i < PROC_FILE_COUNT; i++) {
        CHECK_EQ(proc_files[i].flags, FS_FILE);
        CHECK(proc_files[i].read != NULL);
        CHECK(proc_files[i].write == NULL);
    }
}

static void test_readdir(void) {
    fs_node_t *root = procfs_get_root();
    dirent_t *entry;

    TEST("readdir");
    reset_all();

    /* Exactly four entries, in order, then a hard stop. The shell's `ls`
     * walks indices until NULL, so an off-by-one here either hides "self" or
     * runs off the name table. */
    entry = root->readdir(root, 0);
    CHECK(entry != NULL);
    if (entry) CHECK_STREQ(entry->name, "count");
    entry = root->readdir(root, 1);
    if (entry) CHECK_STREQ(entry->name, "uptime");
    entry = root->readdir(root, 2);
    if (entry) CHECK_STREQ(entry->name, "processes");
    entry = root->readdir(root, 3);
    if (entry) CHECK_STREQ(entry->name, "self");

    CHECK(root->readdir(root, 4) == NULL);
    CHECK(root->readdir(root, 5) == NULL);
    CHECK(root->readdir(root, 0xFFFFFFFFu) == NULL);

    /* The "self" subdirectory lists its own two files and stops. */
    {
        fs_node_t *self = root->finddir(root, "self");

        CHECK(self != NULL);
        if (self) {
            CHECK_EQ(self->flags, FS_DIRECTORY);
            entry = self->readdir(self, 0);
            if (entry) CHECK_STREQ(entry->name, "name");
            entry = self->readdir(self, 1);
            if (entry) CHECK_STREQ(entry->name, "status");
            CHECK(self->readdir(self, 2) == NULL);
            CHECK(self->readdir(self, 0xFFFFFFFFu) == NULL);
        }
    }
}

static void test_finddir(void) {
    fs_node_t *root = procfs_get_root();
    fs_node_t *node;

    TEST("finddir");
    reset_all();

    node = root->finddir(root, "uptime");
    CHECK(node != NULL);
    if (node) CHECK_EQ(node->flags, FS_FILE);

    CHECK(root->finddir(root, "count") != NULL);
    CHECK(root->finddir(root, "processes") != NULL);
    CHECK(root->finddir(root, "self") != NULL);

    /* Unknown names, and near-misses that a sloppy prefix match would accept. */
    CHECK(root->finddir(root, "nope") == NULL);
    CHECK(root->finddir(root, "coun") == NULL);
    CHECK(root->finddir(root, "counts") == NULL);
    CHECK(root->finddir(root, "") == NULL);
    CHECK(root->finddir(root, "Count") == NULL);   /* case-sensitive */

    {
        fs_node_t *self = root->finddir(root, "self");

        CHECK(self->finddir(self, "name") != NULL);
        CHECK(self->finddir(self, "status") != NULL);
        CHECK(self->finddir(self, "self") == NULL);
        CHECK(self->finddir(self, "count") == NULL);
    }

    /* Looking a file up refreshes its advertised size, which is what stat()
     * reports. With three processes the count file holds "3\n". */
    set_snapshot(3);
    node = root->finddir(root, "count");
    CHECK(node != NULL);
    if (node) CHECK_EQ(node->length, 2);
}

/* --- content -------------------------------------------------------------- */

static void test_count(void) {
    TEST("/proc/count");
    reset_all();

    CHECK_EQ(generate(PROC_COUNT), 2);
    CHECK_STREQ(g_out, "0\n");

    set_snapshot(1);
    CHECK_EQ(generate(PROC_COUNT), 2);
    CHECK_STREQ(g_out, "1\n");

    set_snapshot(MAX_PROCESSES);
    CHECK_EQ(generate(PROC_COUNT), 3);
    CHECK_STREQ(g_out, "16\n");
}

static void test_uptime(void) {
    TEST("/proc/uptime");
    reset_all();

    CHECK_EQ(generate(PROC_UPTIME), 2);
    CHECK_STREQ(g_out, "0\n");

    g_ticks = 1;
    CHECK_STREQ((generate(PROC_UPTIME), g_out), "1\n");

    g_ticks = 100;
    CHECK_STREQ((generate(PROC_UPTIME), g_out), "100\n");

    /* The widest a tick count can get. It takes ~497 days at 100 Hz to reach
     * this, so no boot will -- but it is the value the formatter has to
     * survive, and ten digits is what the /proc/processes bound reserves per
     * field on the strength of exactly this. */
    g_ticks = 0xFFFFFFFFu;
    CHECK_EQ(generate(PROC_UPTIME), 11);
    CHECK_STREQ(g_out, "4294967295\n");
}

static void test_processes(void) {
    TEST("/proc/processes");
    reset_all();

    /* No processes: an empty file, not a blank line. */
    CHECK_EQ(generate(PROC_PROCESSES), 0);

    add_process(0, 1, 0, PROCESS_RUNNING, "shell");
    set_snapshot(1);
    CHECK_EQ(generate(PROC_PROCESSES), 12);
    CHECK_STREQ(g_out, "1 0 R shell\n");

    /* A zombie renders as Z; everything else as R. The shell's `ps` and the
     * suite's "R cat" assertion both depend on this single character. */
    add_process(1, 2, 1, PROCESS_ZOMBIE, "cat");
    set_snapshot(2);
    generate(PROC_PROCESSES);
    CHECK_STREQ(g_out, "1 0 R shell\n2 1 Z cat\n");

    /* A process with an empty name still produces a well-formed line. */
    add_process(0, 7, 3, PROCESS_RUNNING, "");
    set_snapshot(1);
    generate(PROC_PROCESSES);
    CHECK_STREQ(g_out, "7 3 R \n");

    /* Every process in the table, with ordinary short fields: all sixteen
     * lines must be present. The bound reserves the worst case per line, so
     * an over-eager check would truncate a listing that fits comfortably. */
    reset_all();
    for (uint32_t i = 0; i < MAX_PROCESSES; i++)
        add_process(i, (int32_t)i + 1, 1, PROCESS_RUNNING, "prog");
    set_snapshot(MAX_PROCESSES);
    {
        uint32_t len = generate(PROC_PROCESSES);
        int lines = 0;

        for (uint32_t i = 0; i < len; i++) if (g_out[i] == '\n') lines++;
        CHECK_EQ(lines, MAX_PROCESSES);
        CHECK(len < sizeof(gen_buf));
    }
}

static void test_processes_truncation(void) {
    uint32_t len;
    int lines = 0;

    /*
     * The F3 guard, executed for the first time.
     *
     * Sixteen processes, each with a ten-digit pid and ppid and a name that
     * fills PROCESS_NAME_MAX: 40 bytes a line, 640 bytes in all, against a
     * 512-byte buffer. The generator has to stop early -- and stop cleanly,
     * between lines, because a listing cut mid-number is worse than a short
     * one: the reader cannot tell a truncated pid from a real one.
     *
     * pids get there on their own. next_pid only ever increments, so a
     * long-lived system reaches ten digits without anything unusual
     * happening; this is the state F3's fix exists for.
     */
    TEST("/proc/processes truncation");
    reset_all();
    for (uint32_t i = 0; i < MAX_PROCESSES; i++) {
        add_process(i, (int32_t)0x7FFFFFFF, (int32_t)0x7FFFFFF0,
                    PROCESS_RUNNING, "abcdefghijklmno");
    }
    set_snapshot(MAX_PROCESSES);

    len = generate(PROC_PROCESSES);

    /* Truncated, but by whole lines, and inside the buffer.
     *
     * The exact numbers, because they are the point: each line here is
     * 10 + 1 + 10 + 1 + 1 + 1 + 15 + 1 = 40 bytes, which is precisely what the
     * guard reserves. Twelve fit (480 bytes); a thirteenth would need
     * 480 + 40 = 520 against a 512-byte buffer, so the guard stops there.
     * Pinning 12 rather than "some number less than 16" is what makes this
     * test notice a buffer resize or a change to the reservation. */
    CHECK(len > 0);
    CHECK(len <= sizeof(gen_buf));
    CHECK_EQ(g_out[len - 1], '\n');
    for (uint32_t i = 0; i < len; i++) if (g_out[i] == '\n') lines++;
    CHECK_EQ(lines, 12);
    CHECK(lines < MAX_PROCESSES);      /* it really did stop early */
    CHECK_EQ(len, 480);
    CHECK_EQ(len, (uint32_t)lines * 40);

    /* The line that did not fit was dropped whole -- not started and
     * abandoned. A listing cut mid-number is worse than a short one: nothing
     * distinguishes a truncated pid from a real one. */
    CHECK(len + 40 > sizeof(gen_buf));

    /* Every line must be complete: the same field count throughout. */
    {
        int spaces_in_line = 0;

        for (uint32_t i = 0; i < len; i++) {
            if (g_out[i] == ' ') spaces_in_line++;
            if (g_out[i] == '\n') {
                CHECK_EQ(spaces_in_line, 3);
                spaces_in_line = 0;
            }
        }
    }
}

/* Fill a snapshot slot with a line of a chosen total width: ten-digit pid and
 * ppid plus a name of `name_len`, giving 10+1+10+1+1+1+name_len+1 bytes. */
static void add_sized_process(uint32_t index, int name_len) {
    char name[PROCESS_NAME_MAX];
    int i;

    for (i = 0; i < name_len && i < PROCESS_NAME_MAX - 1; i++) name[i] = 'x';
    name[i] = '\0';
    add_process(index, (int32_t)0x7FFFFFFF, (int32_t)0x7FFFFFF0,
                PROCESS_RUNNING, name);
}

static void test_processes_exact_boundary(void) {
    uint32_t len;
    int lines = 0;

    /*
     * The guard's exact edge, which uniform lines cannot reach.
     *
     * The truncation test above makes every line the full 40 bytes, so `pos`
     * only ever takes values that are multiples of 40 -- and the guard's
     * decision is identical for a whole range of thresholds around each of
     * them. Two mutations of the bound survived that test for precisely this
     * reason: one that allowed a line to start with one byte too little room,
     * and one that under-reserved the name field by seven. Both are real
     * overflows and neither changed a single assertion.
     *
     * So land `pos` on the exact boundary. Eleven full-width lines make 440;
     * a twelfth at 33 bytes (an eight-character name) makes 473. 473 is the
     * largest offset at which the guard must still refuse, because 473 + 40 is
     * 513 -- one past the end of a 512-byte buffer. One byte of slack in
     * either direction and the thirteenth line writes gen_buf[512].
     */
    TEST("/proc/processes bound is exact");
    reset_all();
    for (uint32_t i = 0; i < 11; i++) add_sized_process(i, 15);   /* 40 each */
    add_sized_process(11, 8);                                     /* 33 */
    for (uint32_t i = 12; i < MAX_PROCESSES; i++) add_sized_process(i, 15);
    set_snapshot(MAX_PROCESSES);

    len = generate(PROC_PROCESSES);

    CHECK_EQ(len, 473);
    CHECK_EQ(len + 40, sizeof(gen_buf) + 1);   /* one byte too few, exactly */
    for (uint32_t i = 0; i < len; i++) if (g_out[i] == '\n') lines++;
    CHECK_EQ(lines, 12);
    CHECK_EQ(g_out[len - 1], '\n');

    /* One byte more room and the next line would fit: the same twelve lines
     * with a 34-byte twelfth take pos to 474, which still has to be refused,
     * while a 32-byte twelfth reaches 472 and lets a thirteenth line in. That
     * pair brackets the boundary from both sides, so a test cannot pass by
     * rejecting everything. */
    reset_all();
    for (uint32_t i = 0; i < 11; i++) add_sized_process(i, 15);
    add_sized_process(11, 7);                                     /* 32 */
    for (uint32_t i = 12; i < MAX_PROCESSES; i++) add_sized_process(i, 15);
    set_snapshot(MAX_PROCESSES);

    len = generate(PROC_PROCESSES);
    lines = 0;
    for (uint32_t i = 0; i < len; i++) if (g_out[i] == '\n') lines++;
    CHECK_EQ(lines, 13);                       /* the thirteenth fits */
    CHECK_EQ(len, 512);
    CHECK_EQ(len, sizeof(gen_buf));            /* filling it exactly is legal */
}

static void test_self_files(void) {
    TEST("/proc/self");
    reset_all();

    /* No current process -- the kernel shell's own context. Both files must
     * still render something rather than dereference NULL. */
    g_current = NULL;
    CHECK_EQ(generate(PROC_SELF_NAME), 2);
    CHECK_STREQ(g_out, "?\n");

    generate(PROC_SELF_STATUS);
    CHECK_STREQ(g_out, "name=? pid=0 ppid=0 state=? cpu=0\n");

    g_current = make_process(5, 1, PROCESS_RUNNING, "cat", 42);
    CHECK_EQ(generate(PROC_SELF_NAME), 4);
    CHECK_STREQ(g_out, "cat\n");

    generate(PROC_SELF_STATUS);
    CHECK_STREQ(g_out, "name=cat pid=5 ppid=1 state=R cpu=42\n");

    g_current = make_process(9, 2, PROCESS_ZOMBIE, "gone", 0);
    generate(PROC_SELF_STATUS);
    CHECK_STREQ(g_out, "name=gone pid=9 ppid=2 state=Z cpu=0\n");
}

static void test_self_status_worst_case(void) {
    uint32_t len;

    /*
     * /proc/self/status has no bound check whatsoever -- not a conservative
     * one like /proc/processes, none at all. It is safe today because the
     * arithmetic works out: "name=" + 15 + " pid=" + 10 + " ppid=" + 10 +
     * " state=" + 1 + " cpu=" + 10 + "\n" = 75 bytes, against 512.
     *
     * That is a fact about today's field widths, not something the code
     * enforces. Widen PROCESS_NAME_MAX, or format a 64-bit tick count, and
     * nothing in procfs.c objects. So the worst case is constructed here and
     * measured: if the margin ever narrows, this is what says so.
     */
    TEST("/proc/self/status worst case");
    reset_all();

    g_current = make_process((int32_t)0xFFFFFFFF, (int32_t)0xFFFFFFFF,
                             PROCESS_RUNNING, "abcdefghijklmno", 0xFFFFFFFFu);
    len = generate(PROC_SELF_STATUS);

    CHECK_EQ(len, 75);
    CHECK(len < sizeof(gen_buf));
    CHECK_STREQ(g_out,
                "name=abcdefghijklmno pid=4294967295 ppid=4294967295 "
                "state=R cpu=4294967295\n");

    /* And the widest /proc/self/name, for the same reason. */
    len = generate(PROC_SELF_NAME);
    CHECK_EQ(len, 16);
    CHECK_STREQ(g_out, "abcdefghijklmno\n");
}

static void test_unknown_generator(void) {
    TEST("unknown generator id");
    reset_all();

    /* impl values that match no case: the switch falls through to `default`
     * and produces nothing, rather than leaving `pos` uninitialised or
     * emitting whatever the last caller left in the buffer. */
    CHECK_EQ(generate(0), 0);
    CHECK_EQ(generate(99), 0);
    CHECK_EQ(generate(0xFFFFFFFFu), 0);
}

/* --- read path ------------------------------------------------------------ */

static void test_read_offsets(void) {
    fs_node_t *root = procfs_get_root();
    fs_node_t *node;
    uint8_t buffer[64];

    TEST("read offsets");
    reset_all();
    g_ticks = 12345;                       /* "12345\n", six bytes */
    node = root->finddir(root, "uptime");
    CHECK(node != NULL);
    if (!node) return;

    for (unsigned i = 0; i < sizeof(buffer); i++) buffer[i] = 0xCC;
    CHECK_EQ(node->read(node, 0, sizeof(buffer), buffer), 6);
    CHECK_EQ(buffer[0], '1');
    CHECK_EQ(buffer[5], '\n');
    CHECK_EQ(buffer[6], 0xCC);             /* nothing written past the end */

    /* A short read hands back a prefix and leaves the rest alone. */
    for (unsigned i = 0; i < sizeof(buffer); i++) buffer[i] = 0xCC;
    CHECK_EQ(node->read(node, 0, 2, buffer), 2);
    CHECK_EQ(buffer[0], '1');
    CHECK_EQ(buffer[1], '2');
    CHECK_EQ(buffer[2], 0xCC);

    /* Reading from an offset returns the matching slice -- this is how the
     * shell's `cat` walks a file, so it has to line up. */
    for (unsigned i = 0; i < sizeof(buffer); i++) buffer[i] = 0xCC;
    CHECK_EQ(node->read(node, 3, sizeof(buffer), buffer), 3);
    CHECK_EQ(buffer[0], '4');
    CHECK_EQ(buffer[1], '5');
    CHECK_EQ(buffer[2], '\n');

    /* At and past the end: end of file, and no write into the caller's
     * buffer. An offset that ran off the end would read out of gen_buf. */
    for (unsigned i = 0; i < sizeof(buffer); i++) buffer[i] = 0xCC;
    CHECK_EQ(node->read(node, 6, sizeof(buffer), buffer), 0);
    CHECK_EQ(node->read(node, 7, sizeof(buffer), buffer), 0);
    CHECK_EQ(node->read(node, 0xFFFFFFFFu, sizeof(buffer), buffer), 0);
    CHECK_EQ(buffer[0], 0xCC);

    /* A zero-length read is not an error and touches nothing. */
    CHECK_EQ(node->read(node, 0, 0, buffer), 0);
    CHECK_EQ(buffer[0], 0xCC);
}

static void test_read_is_live(void) {
    fs_node_t *root = procfs_get_root();
    fs_node_t *node;
    uint8_t buffer[64];

    TEST("reads reflect live state");
    reset_all();
    node = root->finddir(root, "count");
    CHECK(node != NULL);
    if (!node) return;

    /* The whole point of /proc: content is generated per read, so a second
     * read after the state changed shows the new state without reopening. */
    set_snapshot(2);
    CHECK_EQ(node->read(node, 0, sizeof(buffer), buffer), 2);
    CHECK_EQ(buffer[0], '2');

    set_snapshot(9);
    CHECK_EQ(node->read(node, 0, sizeof(buffer), buffer), 2);
    CHECK_EQ(buffer[0], '9');

    /* Each read really does re-query rather than reuse a cached snapshot. */
    g_snapshot_calls = 0;
    node->read(node, 0, sizeof(buffer), buffer);
    node->read(node, 0, sizeof(buffer), buffer);
    CHECK_EQ(g_snapshot_calls, 2);
}

int main(void) {
    test_root_shape();
    test_readdir();
    test_finddir();
    test_count();
    test_uptime();
    test_processes();
    test_processes_truncation();
    test_processes_exact_boundary();
    test_self_files();
    test_self_status_worst_case();
    test_unknown_generator();
    test_read_offsets();
    test_read_is_live();
    TEST_REPORT("procfs");
}
