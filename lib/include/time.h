#ifndef NEOOS_TIME_H
#define NEOOS_TIME_H

#include <stdint.h>

typedef int64_t time_t;

// Linux's x86_64 layout exactly: two 64-bit signed fields, tv_sec
// first. Anything compiled against Linux headers passes this struct by
// address, so the layout is part of the ABI and cannot drift -- see
// docs/abi-compatibility.md.
struct timespec {
    time_t  tv_sec;
    int64_t tv_nsec;   // `long` on Linux x86_64, which is 64-bit there too
};

// Linux's clock ids. Every one NeoOS accepts reads the SAME 100Hz tick
// counter, so they differ in name only -- see docs/stdlib.md.
#define CLOCK_REALTIME           0
#define CLOCK_MONOTONIC          1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID  3
#define CLOCK_MONOTONIC_RAW      4

// DIVERGENCE: the epoch is BOOT, not 1970. Differences between two
// readings are correct; an absolute CLOCK_REALTIME formats as a date in
// January 1970. Resolution is one 10ms tick.
int clock_gettime(int clk, struct timespec *out);

// Relative sleep, rounded up to a whole tick. `rem` is accepted and
// ignored: nothing can interrupt a sleep partway yet.
int nanosleep(const struct timespec *req, struct timespec *rem);

#endif
