#include "test.h"

/*
 * The RTC's real work is decoding CMOS register bytes: BCD vs binary, and the
 * 12-hour clock with its PM bit and the 12->0 / 12->noon special cases. That
 * logic is fiddly and, because QEMU is always started at a fixed binary-ish
 * time, the shell `date` test only ever exercises one path through it.
 *
 * cmos_read() reads real ports (no-ops under HOSTED_TEST), so the decode was
 * split out into rtc_decode() to make it testable without a chip. Including the
 * .c gives the test access to that static function and to bcd_to_bin; the port
 * I/O the rest of rtc.c uses is compiled out by HOSTED_TEST.
 */
#include "../rtc.c"

/* regB bits: 0x04 = binary (else BCD); 0x02 = 24-hour (else 12-hour). */
#define BIN24   (0x04 | 0x02)
#define BIN12   (0x04)
#define BCD24   (0x02)
#define BCD12   (0x00)

static void test_bcd(void) {
    TEST("bcd_to_bin");
    CHECK_EQ(bcd_to_bin(0x00), 0);
    CHECK_EQ(bcd_to_bin(0x09), 9);
    CHECK_EQ(bcd_to_bin(0x10), 10);
    CHECK_EQ(bcd_to_bin(0x42), 42);
    CHECK_EQ(bcd_to_bin(0x59), 59);
    CHECK_EQ(bcd_to_bin(0x99), 99);
}

static void test_binary_24h(void) {
    rtc_time_t t;
    TEST("binary, 24-hour");
    rtc_decode(&t, 30, 45, 13, 15, 6, 25, BIN24);
    CHECK_EQ(t.second, 30);
    CHECK_EQ(t.minute, 45);
    CHECK_EQ(t.hour, 13);
    CHECK_EQ(t.day, 15);
    CHECK_EQ(t.month, 6);
    CHECK_EQ(t.year, 2025);            /* century applied */

    rtc_decode(&t, 0, 0, 0, 1, 1, 0, BIN24);
    CHECK_EQ(t.hour, 0);
    CHECK_EQ(t.year, 2000);
    rtc_decode(&t, 59, 59, 23, 31, 12, 99, BIN24);
    CHECK_EQ(t.hour, 23);
    CHECK_EQ(t.year, 2099);
}

static void test_bcd_24h(void) {
    rtc_time_t t;
    TEST("BCD, 24-hour");
    /* Same instant as test_binary_24h but BCD-encoded. */
    rtc_decode(&t, 0x30, 0x45, 0x13, 0x15, 0x06, 0x25, BCD24);
    CHECK_EQ(t.second, 30);
    CHECK_EQ(t.minute, 45);
    CHECK_EQ(t.hour, 13);
    CHECK_EQ(t.day, 15);
    CHECK_EQ(t.month, 6);
    CHECK_EQ(t.year, 2025);
}

static void test_binary_12h(void) {
    rtc_time_t t;

    TEST("binary, 12-hour AM");
    /* 12 AM must map to 0 (midnight). */
    rtc_decode(&t, 0, 0, 12, 1, 1, 24, BIN12);
    CHECK_EQ(t.hour, 0);
    /* 1..11 AM pass through. */
    rtc_decode(&t, 0, 0, 1, 1, 1, 24, BIN12);
    CHECK_EQ(t.hour, 1);
    rtc_decode(&t, 0, 0, 11, 1, 1, 24, BIN12);
    CHECK_EQ(t.hour, 11);

    TEST("binary, 12-hour PM");
    /* PM bit (0x80). 1 PM -> 13. */
    rtc_decode(&t, 0, 0, 0x80 | 1, 1, 1, 24, BIN12);
    CHECK_EQ(t.hour, 13);
    rtc_decode(&t, 0, 0, 0x80 | 11, 1, 1, 24, BIN12);
    CHECK_EQ(t.hour, 23);
    /* 12 PM must stay 12 (noon), not become 24. */
    rtc_decode(&t, 0, 0, 0x80 | 12, 1, 1, 24, BIN12);
    CHECK_EQ(t.hour, 12);
}

static void test_bcd_12h(void) {
    rtc_time_t t;

    TEST("BCD, 12-hour, PM bit preserved through decode");
    /* 3 PM in BCD: hour register = 0x80 | 0x03. */
    rtc_decode(&t, 0, 0, 0x80 | 0x03, 1, 1, 0x24, BCD12);
    CHECK_EQ(t.hour, 15);
    /* 12 PM in BCD: 0x80 | 0x12. */
    rtc_decode(&t, 0, 0, 0x80 | 0x12, 1, 1, 0x24, BCD12);
    CHECK_EQ(t.hour, 12);
    /* 12 AM in BCD: hour 0x12, no PM bit -> 0. */
    rtc_decode(&t, 0x30, 0x00, 0x12, 0x09, 0x08, 0x24, BCD12);
    CHECK_EQ(t.hour, 0);
    CHECK_EQ(t.second, 30);
    CHECK_EQ(t.day, 9);
    CHECK_EQ(t.month, 8);
    CHECK_EQ(t.year, 2024);
}

int main(void) {
    test_bcd();
    test_binary_24h();
    test_bcd_24h();
    test_binary_12h();
    test_bcd_12h();
    TEST_REPORT("rtc");
}
