#include "dev/rtc.h"
#include "dev/timer.h"
#include "arch/io.h"
#include "dev/serial.h"

// The CMOS RTC, behind the index/data port pair at 0x70/0x71.
//
// Everything awkward about this device is in one place: the registers
// may be BCD or binary, hours may be 12- or 24-hour with a PM bit, and
// a read can straddle an update and return a half-carried time. Status
// Register B says which encoding; Status Register A's UIP bit says when
// an update is in flight.

#define CMOS_INDEX 0x70
#define CMOS_DATA  0x71

#define RTC_SECONDS 0x00
#define RTC_MINUTES 0x02
#define RTC_HOURS   0x04
#define RTC_DAY     0x07
#define RTC_MONTH   0x08
#define RTC_YEAR    0x09
#define RTC_CENTURY 0x32
#define RTC_STATUS_A 0x0A
#define RTC_STATUS_B 0x0B

#define STATUS_A_UIP  0x80
#define STATUS_B_24H  0x02
#define STATUS_B_BIN  0x04

static int64_t boot_epoch;
static int     have_rtc;

static uint8_t cmos_read(uint8_t reg) {
    // Bit 7 of the index port is the NMI-disable bit. It is left CLEAR
    // deliberately: masking NMIs here would also mask the panic-stop
    // IPI, which is an NMI (see smp_panic_stop_others).
    outb(CMOS_INDEX, reg);
    return inb(CMOS_DATA);
}

static int update_in_progress(void) {
    return cmos_read(RTC_STATUS_A) & STATUS_A_UIP;
}

static uint8_t bcd_to_bin(uint8_t v) {
    return (uint8_t)((v & 0x0F) + ((v >> 4) * 10));
}

struct rtc_time {
    int sec, min, hour, day, month, year;
};

// Reads every field, twice, and only accepts a pair of identical
// readings taken outside an update.
//
// A single read is not enough and the UIP check alone is not enough
// either: the clock can begin updating between two of the register
// reads, so the value can carry (23:59:59 -> 00:00:00) mid-read and
// produce a timestamp a day out. Reading until two consecutive passes
// agree is the standard fix.
static int rtc_read_raw(struct rtc_time *out) {
    struct rtc_time a, b;
    uint8_t status_b;

    for (int attempt = 0; attempt < 100; attempt++) {
        int guard = 0;
        while (update_in_progress() && guard++ < 1000000) { }

        a.sec   = cmos_read(RTC_SECONDS);
        a.min   = cmos_read(RTC_MINUTES);
        a.hour  = cmos_read(RTC_HOURS);
        a.day   = cmos_read(RTC_DAY);
        a.month = cmos_read(RTC_MONTH);
        a.year  = cmos_read(RTC_YEAR);
        int century = cmos_read(RTC_CENTURY);

        guard = 0;
        while (update_in_progress() && guard++ < 1000000) { }

        b.sec   = cmos_read(RTC_SECONDS);
        b.min   = cmos_read(RTC_MINUTES);
        b.hour  = cmos_read(RTC_HOURS);
        b.day   = cmos_read(RTC_DAY);
        b.month = cmos_read(RTC_MONTH);
        b.year  = cmos_read(RTC_YEAR);

        if (a.sec != b.sec || a.min != b.min || a.hour != b.hour ||
            a.day != b.day || a.month != b.month || a.year != b.year) {
            continue;   // straddled an update; take it again
        }

        status_b = cmos_read(RTC_STATUS_B);

        if (!(status_b & STATUS_B_BIN)) {
            // The PM bit must be stripped BEFORE the BCD conversion,
            // or 0x80 corrupts the tens digit.
            int pm = (!(status_b & STATUS_B_24H)) && (a.hour & 0x80);
            a.hour &= 0x7F;
            a.sec   = bcd_to_bin((uint8_t)a.sec);
            a.min   = bcd_to_bin((uint8_t)a.min);
            a.hour  = bcd_to_bin((uint8_t)a.hour);
            a.day   = bcd_to_bin((uint8_t)a.day);
            a.month = bcd_to_bin((uint8_t)a.month);
            a.year  = bcd_to_bin((uint8_t)a.year);
            century = bcd_to_bin((uint8_t)century);
            if (pm && a.hour != 12) { a.hour += 12; }
            if (!pm && a.hour == 12 && !(status_b & STATUS_B_24H)) { a.hour = 0; }
        } else if (!(status_b & STATUS_B_24H)) {
            int pm = a.hour & 0x80;
            a.hour &= 0x7F;
            if (pm && a.hour != 12) { a.hour += 12; }
            if (!pm && a.hour == 12) { a.hour = 0; }
        }

        // The century register is optional and reads as 0 or 0xFF on
        // machines that do not have it. Fall back to the 2000s, which
        // is right for anything this code will run on.
        if (century >= 19 && century <= 21) {
            a.year += century * 100;
        } else {
            a.year += 2000;
        }

        *out = a;
        return 1;
    }
    return 0;
}

static int is_leap(int y) {
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

// Days from the Unix epoch to the first of the given month.
static int64_t days_from_epoch(int year, int month, int day) {
    static const int cumulative[12] = { 0, 31, 59, 90, 120, 151,
                                        181, 212, 243, 273, 304, 334 };
    int64_t days = 0;
    for (int y = 1970; y < year; y++) { days += is_leap(y) ? 366 : 365; }
    days += cumulative[(month - 1) % 12];
    if (month > 2 && is_leap(year)) { days += 1; }
    days += day - 1;
    return days;
}

int64_t rtc_time_to_epoch(int year, int month, int day, int hour, int min, int sec) {
    return days_from_epoch(year, month, day) * 86400
         + (int64_t)hour * 3600 + (int64_t)min * 60 + sec;
}

void rtc_init(void) {
    struct rtc_time t;
    if (!rtc_read_raw(&t)) {
        serial_write_string("[rtc] FAILED: could not read a stable time; "
                            "the wall clock will start at the epoch\n");
        boot_epoch = 0;
        have_rtc = 0;
        return;
    }

    // Sanity: the RTC is battery-backed hardware and can be unset or
    // nonsense. A year outside this range means the value should not
    // be trusted as a wall clock.
    if (t.year < 1970 || t.year > 2200 || t.month < 1 || t.month > 12 ||
        t.day < 1 || t.day > 31 || t.hour > 23 || t.min > 59 || t.sec > 60) {
        serial_write_string("[rtc] FAILED: implausible time; "
                            "the wall clock will start at the epoch\n");
        boot_epoch = 0;
        have_rtc = 0;
        return;
    }

    int64_t now = rtc_time_to_epoch(t.year, t.month, t.day, t.hour, t.min, t.sec);
    // Anchor at TICK ZERO, not at now: callers add the current tick
    // count, and some ticks have already passed by the time this runs.
    boot_epoch = now - (int64_t)(timer_ticks() / TIMER_HZ);
    have_rtc = 1;

    serial_write_string("[rtc] wall clock set, epoch=");
    serial_write_hex64((uint64_t)now);
    serial_write_string(" utc=");
    serial_write_hex64((uint64_t)t.year);
    serial_write_string("-");
    serial_write_hex64((uint64_t)t.month);
    serial_write_string("-");
    serial_write_hex64((uint64_t)t.day);
    serial_write_string("\n");
}

int64_t rtc_boot_epoch(void) { return boot_epoch; }
int     rtc_is_real(void)    { return have_rtc; }

void rtc_selftest(void) {
    // Fixed points, so the calendar arithmetic is checked rather than
    // assumed. These are the values `date -u -d ... +%s` reports.
    struct { int y, mo, d, h, mi, s; int64_t want; } cases[] = {
        { 1970,  1,  1,  0,  0,  0,          0 },
        { 1970,  1,  2,  0,  0,  0,      86400 },
        { 2000,  1,  1,  0,  0,  0,  946684800 },   // across 1972..1996 leaps
        { 2000,  3,  1,  0,  0,  0,  951868800 },   // 2000 IS a leap year
        { 2024,  2, 29, 12, 30, 15, 1709209815 },   // leap day
        { 2026,  8, 31,  0,  0,  0, 1788134400 },
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int64_t got = rtc_time_to_epoch(cases[i].y, cases[i].mo, cases[i].d,
                                        cases[i].h, cases[i].mi, cases[i].s);
        if (got != cases[i].want) {
            serial_write_string("[rtc] selftest FAILED: case ");
            serial_write_hex64(i);
            serial_write_string(" got ");
            serial_write_hex64((uint64_t)got);
            serial_write_string(" want ");
            serial_write_hex64((uint64_t)cases[i].want);
            serial_write_string("\n");
            return;
        }
    }
    serial_write_string("[rtc] selftest passed\n");
}
