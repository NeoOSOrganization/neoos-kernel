#ifndef NEOOS_KTIME_H
#define NEOOS_KTIME_H
#include <stdint.h>

#define NSEC_PER_SEC 1000000000ULL
#define TICK_NS      10000000ULL      // the legacy 100 Hz tick, for timer_ticks()

// Nanoseconds since boot from the TSC. The one time base of the kernel.
uint64_t ktime_get_ns(void);
// now + ns / now + ticks*10ms, saturating at UINT64_MAX ("forever").
uint64_t ktime_after_ns(uint64_t ns);
uint64_t ktime_after_ticks(uint64_t ticks);
// A user timespec's value in ns, saturating at UINT64_MAX ("forever")
// the way Linux clamps to KTIME_MAX. sec and nsec must be validated
// non-negative by the caller.
static inline uint64_t ktime_ts_to_ns(uint64_t sec, uint64_t nsec) {
    if (sec >= UINT64_MAX / NSEC_PER_SEC) { return UINT64_MAX; }
    uint64_t ns = sec * NSEC_PER_SEC;
    return (ns + nsec < ns) ? UINT64_MAX : ns + nsec;
}
// Absolute ktime ns -> absolute TSC value (for TSC-deadline).
uint64_t ktime_ns_to_tsc(uint64_t ns);
// Called once by timer_init with the PIT-measured TSC rate.
void ktime_calibrate(uint64_t tsc_per_10ms);
#endif
