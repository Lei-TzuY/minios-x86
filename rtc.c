#include "rtc.h"
#include "io.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

#define RTC_SECONDS 0x00
#define RTC_MINUTES 0x02
#define RTC_HOURS   0x04
#define RTC_DAY     0x07
#define RTC_MONTH   0x08
#define RTC_YEAR    0x09
#define RTC_STATUS_A 0x0A
#define RTC_STATUS_B 0x0B

static uint8_t cmos_read(uint8_t reg) {
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static int update_in_progress(void) {
    return cmos_read(RTC_STATUS_A) & 0x80;
}

static uint8_t bcd_to_bin(uint8_t v) {
    return (uint8_t)((v & 0x0F) + (v >> 4) * 10);
}

/*
 * Turn the raw RTC register bytes into a normalised rtc_time_t. Kept separate
 * from the hardware read so the fiddly encoding rules can be unit-tested
 * without a CMOS chip. `regB` is RTC status register B:
 *   bit 2 (0x04) set -> registers are already binary; clear -> BCD-encoded.
 *   bit 1 (0x02) set -> 24-hour clock; clear -> 12-hour, with bit 7 of the
 *                       hour register meaning PM.
 * `hour_raw` is the hour register with its PM bit still attached.
 */
static void rtc_decode(rtc_time_t *out, uint8_t second, uint8_t minute,
                       uint8_t hour_raw, uint8_t day, uint8_t month,
                       uint8_t year, uint8_t regB) {
    rtc_time_t t;

    t.second = second;
    t.minute = minute;
    t.day    = day;
    t.month  = month;
    t.year   = year;

    if (!(regB & 0x04)) {   /* values are BCD-encoded */
        t.second = bcd_to_bin(t.second);
        t.minute = bcd_to_bin(t.minute);
        t.day    = bcd_to_bin(t.day);
        t.month  = bcd_to_bin(t.month);
        t.year   = bcd_to_bin((uint8_t)t.year);
        /* Keep bit 7 (PM flag) while converting the low bits. */
        hour_raw = (uint8_t)(bcd_to_bin(hour_raw & 0x7F) | (hour_raw & 0x80));
    }

    t.hour = hour_raw & 0x7F;
    if (!(regB & 0x02) && (hour_raw & 0x80)) {   /* 12-hour mode, PM */
        t.hour = (uint8_t)((t.hour % 12) + 12);
    } else if (!(regB & 0x02) && t.hour == 12) { /* 12 AM -> 0 */
        t.hour = 0;
    }

    t.year = (uint16_t)(2000 + t.year);   /* 21st-century base */
    *out = t;
}

/* Snapshot all the time registers once the UIP flag is clear. */
static void read_registers(rtc_time_t *t, uint8_t *hour_raw) {
    while (update_in_progress()) { }
    t->second = cmos_read(RTC_SECONDS);
    t->minute = cmos_read(RTC_MINUTES);
    *hour_raw = cmos_read(RTC_HOURS);
    t->day    = cmos_read(RTC_DAY);
    t->month  = cmos_read(RTC_MONTH);
    t->year   = cmos_read(RTC_YEAR);
}

void rtc_read(rtc_time_t *out) {
    rtc_time_t cur, prev;
    uint8_t hour_raw, prev_hour, regB;

    if (!out) return;

    /* Read repeatedly until two consecutive snapshots agree, so we never
     * return a value torn across an RTC update. */
    read_registers(&cur, &hour_raw);
    do {
        prev = cur;
        prev_hour = hour_raw;
        read_registers(&cur, &hour_raw);
    } while (cur.second != prev.second || cur.minute != prev.minute ||
             hour_raw != prev_hour || cur.day != prev.day ||
             cur.month != prev.month || cur.year != prev.year);

    regB = cmos_read(RTC_STATUS_B);

    rtc_decode(out, cur.second, cur.minute, hour_raw, cur.day, cur.month,
               (uint8_t)cur.year, regB);
}
