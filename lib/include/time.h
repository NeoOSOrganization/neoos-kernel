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

// Deliberately NO clock ids and no clock_gettime() yet. NeoOS has a
// tick counter but exposes no clock syscall, and declaring CLOCK_*
// constants for a call that does not exist invites code that compiles
// and then fails at run time. They arrive with the clock milestone.

#endif
