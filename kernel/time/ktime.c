// kernel/time/ktime.c -- the kernel's single time base: nanoseconds
// since boot from the TSC, calibrated against the PIT at boot.
// See docs/superpowers/specs/2026-09-23-hrtimers-design.md section 1.
#include "time/ktime.h"
#include "arch/cpu.h"

static uint64_t tsc_base, tsc_per_10ms;

void ktime_calibrate(uint64_t per_10ms) {
    tsc_per_10ms = per_10ms;
    tsc_base = rdtsc();
}

uint64_t ktime_get_ns(void) {
    uint64_t per = tsc_per_10ms;
    if (per == 0) { return 0; }                 // pre-calibration
    uint64_t d = rdtsc() - tsc_base;
    return (uint64_t)((__uint128_t)d * TICK_NS / per);
}

uint64_t ktime_ns_to_tsc(uint64_t ns) {
    return tsc_base + (uint64_t)((__uint128_t)ns * tsc_per_10ms / TICK_NS);
}

static uint64_t sat_add(uint64_t a, uint64_t b) { return (a + b < a) ? UINT64_MAX : a + b; }
uint64_t ktime_after_ns(uint64_t ns)       { return sat_add(ktime_get_ns(), ns); }
uint64_t ktime_after_ticks(uint64_t ticks) {
    if (ticks > UINT64_MAX / TICK_NS) { return UINT64_MAX; }
    return sat_add(ktime_get_ns(), ticks * TICK_NS);
}
