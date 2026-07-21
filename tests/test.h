#ifndef MINIOS_TEST_H
#define MINIOS_TEST_H

/*
 * Minimal assertion helpers for the native unit tests.
 *
 * These tests compile kernel modules for the host (32-bit, to match the
 * kernel's pointer size) and call them directly, so pure-logic code can be
 * exercised in milliseconds instead of through the multi-minute QEMU suite --
 * and with edge cases that are impractical to reach from the shell.
 *
 * Each test binary includes this header once and ends with TEST_REPORT.
 */

#include <stdio.h>

static int test_checks;
static int test_failures;
static const char *test_current = "?";

#define TEST(name) (test_current = (name))

#define CHECK(cond)                                                          \
    do {                                                                     \
        test_checks++;                                                       \
        if (!(cond)) {                                                       \
            test_failures++;                                                 \
            printf("  FAIL [%s] %s:%d: %s\n",                                \
                   test_current, __FILE__, __LINE__, #cond);                 \
        }                                                                    \
    } while (0)

#define CHECK_EQ(actual, expected)                                           \
    do {                                                                     \
        long a_ = (long)(actual), e_ = (long)(expected);                     \
        test_checks++;                                                       \
        if (a_ != e_) {                                                      \
            test_failures++;                                                 \
            printf("  FAIL [%s] %s:%d: %s -> %ld, want %ld\n",               \
                   test_current, __FILE__, __LINE__, #actual, a_, e_);       \
        }                                                                    \
    } while (0)

#define CHECK_STREQ(actual, expected)                                        \
    do {                                                                     \
        const char *a_ = (actual), *e_ = (expected);                         \
        test_checks++;                                                       \
        if (__builtin_strcmp(a_, e_) != 0) {                                 \
            test_failures++;                                                 \
            printf("  FAIL [%s] %s:%d: %s -> \"%s\", want \"%s\"\n",         \
                   test_current, __FILE__, __LINE__, #actual, a_, e_);       \
        }                                                                    \
    } while (0)

#define TEST_REPORT(suite)                                                   \
    do {                                                                     \
        printf("%-14s %4d checks, %d failure%s\n", (suite), test_checks,     \
               test_failures, test_failures == 1 ? "" : "s");                \
        return test_failures != 0;                                           \
    } while (0)

#endif
