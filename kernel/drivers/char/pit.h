#ifndef NEOOS_PIT_H
#define NEOOS_PIT_H

#include <stdint.h>

// Calibrates the LAPIC timer against a 10ms PIT interval. If
// tsc_per_10ms_out is non-NULL, also stores the TSC delta over the same
// interval so the scheduler can build a nanosecond clock from rdtsc.
uint32_t pit_calibrate_lapic_ticks_per_10ms(uint64_t *tsc_per_10ms_out);

#endif
