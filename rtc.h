#ifndef RTC_H
#define RTC_H

#include <stdint.h>

/* Wall-clock time read from the CMOS real-time clock. Layout is shared with
 * the user-space mirror in user_syscall.h, so keep the field order stable. */
typedef struct rtc_time {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
} rtc_time_t;

/* Read the current date/time from the CMOS RTC (handles the update-in-progress
 * flag, and BCD / 12-hour encodings reported by status register B). */
void rtc_read(rtc_time_t *out);

#endif
