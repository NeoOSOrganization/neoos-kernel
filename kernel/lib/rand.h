#ifndef NEOOS_LIB_RAND_H
#define NEOOS_LIB_RAND_H

#include <stdint.h>
#include <stddef.h>

// Initialize the CSPRNG from RTC, TSC, stack address, and RDRAND (if available).
// Must be called once on the BSP before other CPUs are started.
void rand_init(void);

// Draw random bytes into buf. Thread-safe via a spinlock (called from spawn on any CPU).
void rand_bytes(void *buf, uint64_t n);

// Return a 64-bit random value. Thread-safe.
uint64_t rand_u64(void);

// Self-test the CSPRNG: verify it's not all-zeros, not sequential, and not repeating.
void rand_selftest(void);

#endif
