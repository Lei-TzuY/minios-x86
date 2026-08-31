#include <stddef.h>
#include "../process.h"
#include "../task.h"

/*
 * Per-process environment variables: setenv (overwrite-or-append with a bound)
 * and getenv (lookup with a bounded, truncating copy into the caller's buffer).
 * The `export`/`printenv`/`$VAR` end-to-end test only stores one short variable,
 * so the bound checks, the ENV_MAX cap, the overwrite path and the truncating
 * copy are all unexercised -- and a bounded string copy with a `max - 1` is a
 * classic off-by-one.
 *
 * process.c is huge and coupled, but the env functions only reach
 * process_get_current() -> task_get_current(). Including the .c and building
 * with --gc-sections drops every other function (fork, exec, signals, ...) and
 * their dependencies, so only task_get_current and the string helpers need
 * stubbing.
 */

/* The task/process the env calls operate on. */
static process_t g_proc;
static task_t g_task;

task_t *task_get_current(void) { return &g_task; }

/* String helpers process.c uses (utils.c is not linked). */
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}
void *memcpy(void *d, const void *s, size_t n) {
    unsigned char *dd = d; const unsigned char *ss = s;
    for (size_t i = 0; i < n; i++) dd[i] = ss[i];
    return d;
}

#include "../process.c"

#include "test.h"

static void reset(void) {
    g_task.process = &g_proc;
    g_proc.env_count = 0;
}

static void test_set_get_basic(void) {
    char buf[ENV_VAL_MAX];
    reset();

    TEST("set then get");
    CHECK_EQ(process_setenv("PATH", "/bin"), 0);
    CHECK_EQ(g_proc.env_count, 1);
    CHECK_EQ(process_getenv("PATH", buf, sizeof(buf)), 4);
    CHECK_STREQ(buf, "/bin");

    TEST("missing key");
    CHECK_EQ(process_getenv("NOPE", buf, sizeof(buf)), -1);

    TEST("second distinct key appends");
    CHECK_EQ(process_setenv("HOME", "/root"), 0);
    CHECK_EQ(g_proc.env_count, 2);
    CHECK_EQ(process_getenv("HOME", buf, sizeof(buf)), 5);
    CHECK_STREQ(buf, "/root");
    /* the first key is untouched */
    CHECK_EQ(process_getenv("PATH", buf, sizeof(buf)), 4);
    CHECK_STREQ(buf, "/bin");
}

static void test_overwrite(void) {
    char buf[ENV_VAL_MAX];
    reset();

    TEST("overwrite existing key does not grow the table");
    CHECK_EQ(process_setenv("K", "one"), 0);
    CHECK_EQ(process_setenv("K", "two"), 0);
    CHECK_EQ(g_proc.env_count, 1);
    CHECK_EQ(process_getenv("K", buf, sizeof(buf)), 3);
    CHECK_STREQ(buf, "two");

    /* Overwrite with a shorter value: no stale tail left behind. */
    CHECK_EQ(process_setenv("K", "x"), 0);
    CHECK_EQ(process_getenv("K", buf, sizeof(buf)), 1);
    CHECK_STREQ(buf, "x");
}

static void test_validation(void) {
    char bigk[ENV_KEY_MAX + 8];
    char bigv[ENV_VAL_MAX + 8];
    int i;
    reset();

    TEST("argument validation");
    CHECK_EQ(process_setenv(NULL, "v"), -1);
    CHECK_EQ(process_setenv("k", NULL), -1);
    CHECK_EQ(process_setenv("", "v"), -1);           /* empty key */
    CHECK_EQ(g_proc.env_count, 0);

    for (i = 0; i < ENV_KEY_MAX + 4; i++) bigk[i] = 'k';
    bigk[ENV_KEY_MAX + 4] = '\0';
    for (i = 0; i < ENV_VAL_MAX + 4; i++) bigv[i] = 'v';
    bigv[ENV_VAL_MAX + 4] = '\0';

    CHECK_EQ(process_setenv(bigk, "v"), -1);         /* key too long */
    CHECK_EQ(process_setenv("k", bigv), -1);         /* value too long */
    CHECK_EQ(g_proc.env_count, 0);

    /* The longest ACCEPTED key/value: length ENV_*_MAX - 1. */
    {
        char okk[ENV_KEY_MAX], okv[ENV_VAL_MAX], buf[ENV_VAL_MAX];
        for (i = 0; i < ENV_KEY_MAX - 1; i++) okk[i] = 'a';
        okk[ENV_KEY_MAX - 1] = '\0';
        for (i = 0; i < ENV_VAL_MAX - 1; i++) okv[i] = 'b';
        okv[ENV_VAL_MAX - 1] = '\0';
        CHECK_EQ(process_setenv(okk, okv), 0);
        CHECK_EQ(process_getenv(okk, buf, sizeof(buf)), ENV_VAL_MAX - 1);
    }
}

static void test_capacity(void) {
    char key[8], buf[ENV_VAL_MAX];
    int i;
    reset();

    TEST("ENV_MAX cap");
    for (i = 0; i < ENV_MAX; i++) {
        key[0] = 'e'; key[1] = (char)('0' + i / 10); key[2] = (char)('0' + i % 10);
        key[3] = '\0';
        CHECK_EQ(process_setenv(key, "v"), 0);
    }
    CHECK_EQ(g_proc.env_count, ENV_MAX);
    CHECK_EQ(process_setenv("overflow", "v"), -1);   /* full: rejected */
    CHECK_EQ(g_proc.env_count, ENV_MAX);
    /* An overwrite of an EXISTING key still works when full. */
    CHECK_EQ(process_setenv("e00", "w"), 0);
    CHECK_EQ(process_getenv("e00", buf, sizeof(buf)), 1);
    CHECK_STREQ(buf, "w");
}

static void test_getenv_truncation(void) {
    char small[4];
    reset();

    TEST("getenv truncates into a small buffer");
    CHECK_EQ(process_setenv("LONG", "abcdefgh"), 0);
    /* size 4 -> at most 3 chars plus NUL; return value is the truncated length. */
    CHECK_EQ(process_getenv("LONG", small, sizeof(small)), 3);
    CHECK_STREQ(small, "abc");

    /* size 1 -> empty string, length 0, still null-terminated. */
    {
        char one[1];
        CHECK_EQ(process_getenv("LONG", one, sizeof(one)), 0);
        CHECK_EQ(one[0], '\0');
    }
}

int main(void) {
    test_set_get_basic();
    test_overwrite();
    test_validation();
    test_capacity();
    test_getenv_truncation();
    TEST_REPORT("process-env");
}
