#ifndef NEOOS_RTC_H
#define NEOOS_RTC_H

#include <stdint.h>

// Reads the CMOS real-time clock once and anchors the wall clock to it.
// Call after the timer is running: the anchor is stored as "epoch
// seconds at tick zero", so it needs the tick counter to already mean
// something.
void rtc_init(void);

// Seconds since the Unix epoch at boot (tick 0). 0 if the RTC could not
// be read, which is what makes CLOCK_REALTIME fall back to a boot
// epoch -- see docs/stdlib.md.
int64_t rtc_boot_epoch(void);

// 1 if the RTC was read successfully, 0 if the wall clock is a fiction.
int rtc_is_real(void);

int64_t rtc_time_to_epoch(int year, int month, int day, int hour, int min, int sec);

void rtc_selftest(void);

#endif
