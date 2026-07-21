#include "test.h"
#include "../utils.h"

/*
 * utils.c carries the kernel's memcpy/memset, which were rewritten to copy a
 * word at a time once the pointers are aligned, with byte loops for the
 * unaligned head and tail. That shape has four distinct paths (aligned or not,
 * for source and destination independently) and off-by-one risk at both ends,
 * none of which the end-to-end suite can single out -- it only ever sees
 * "the filesystem still works".
 *
 * So sweep every offset/length combination and check the bytes outside the
 * requested range are untouched as well, which is what catches a head/tail
 * loop that runs one byte too far.
 */

#define PAD  16
#define BUF  96

static unsigned char dst[BUF];
static unsigned char src[BUF];

/* Fill the whole buffer with a known pattern so stray writes are visible. */
static void poison(unsigned char *b) {
    for (int i = 0; i < BUF; i++) b[i] = (unsigned char)(0xC0 + (i & 0x0F));
}

static void test_memset_sweep(void) {
    TEST("memset sweep");
    for (int off = 0; off < 8; off++) {
        for (int len = 0; len <= 40; len++) {
            poison(dst);
            void *ret = memset(dst + PAD + off, 0x5A, (size_t)len);
            CHECK(ret == dst + PAD + off);

            for (int i = 0; i < len; i++)
                CHECK_EQ(dst[PAD + off + i], 0x5A);
            /* Byte immediately before and after must be untouched. */
            CHECK_EQ(dst[PAD + off - 1],
                     (unsigned char)(0xC0 + ((PAD + off - 1) & 0x0F)));
            CHECK_EQ(dst[PAD + off + len],
                     (unsigned char)(0xC0 + ((PAD + off + len) & 0x0F)));
        }
    }
}

static void test_memset_high_bit(void) {
    /* The word-at-a-time path replicates the byte via v * 0x01010101; a value
     * with the top bit set catches sign-extension mistakes in that step. */
    TEST("memset 0xFF");
    poison(dst);
    memset(dst + PAD, 0xFF, 33);
    for (int i = 0; i < 33; i++) CHECK_EQ(dst[PAD + i], 0xFF);
    CHECK_EQ(dst[PAD + 33], (unsigned char)(0xC0 + ((PAD + 33) & 0x0F)));

    /* memset truncates the int value to a byte. */
    poison(dst);
    memset(dst + PAD, 0x1234, 8);
    for (int i = 0; i < 8; i++) CHECK_EQ(dst[PAD + i], 0x34);
}

static void test_memcpy_sweep(void) {
    /* Both alignments vary independently: the fast path is only taken when
     * source and destination agree, so this covers all four combinations. */
    TEST("memcpy sweep");
    for (int soff = 0; soff < 8; soff++) {
        for (int doff = 0; doff < 8; doff++) {
            for (int len = 0; len <= 33; len++) {
                for (int i = 0; i < BUF; i++) src[i] = (unsigned char)(i * 7 + 1);
                poison(dst);

                void *ret = memcpy(dst + PAD + doff, src + PAD + soff,
                                   (size_t)len);
                CHECK(ret == dst + PAD + doff);

                for (int i = 0; i < len; i++)
                    CHECK_EQ(dst[PAD + doff + i], src[PAD + soff + i]);
                CHECK_EQ(dst[PAD + doff - 1],
                         (unsigned char)(0xC0 + ((PAD + doff - 1) & 0x0F)));
                CHECK_EQ(dst[PAD + doff + len],
                         (unsigned char)(0xC0 + ((PAD + doff + len) & 0x0F)));
            }
        }
    }
}

static void test_str_helpers(void) {
    char buf[32];

    TEST("strlen");
    CHECK_EQ(strlen(""), 0);
    CHECK_EQ(strlen("a"), 1);
    CHECK_EQ(strlen("hello"), 5);

    TEST("strcmp");
    CHECK_EQ(strcmp("", ""), 0);
    CHECK_EQ(strcmp("abc", "abc"), 0);
    CHECK(strcmp("abc", "abd") < 0);
    CHECK(strcmp("abd", "abc") > 0);
    CHECK(strcmp("ab", "abc") < 0);
    CHECK(strcmp("abc", "ab") > 0);
    /* Comparison must be unsigned: 0x80 is greater than 'a', not less. */
    CHECK(strcmp("\x80", "a") > 0);

    TEST("strcpy");
    CHECK(strcpy(buf, "miniOS") == buf);
    CHECK_STREQ(buf, "miniOS");
    CHECK(strcpy(buf, "") == buf);
    CHECK_STREQ(buf, "");
}

int main(void) {
    test_memset_sweep();
    test_memset_high_bit();
    test_memcpy_sweep();
    test_str_helpers();
    TEST_REPORT("utils");
}
