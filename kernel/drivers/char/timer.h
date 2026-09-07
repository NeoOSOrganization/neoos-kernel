#ifndef NEOOS_TIMER_H
#define NEOOS_TIMER_H

#include <stdint.h>

#define VECTOR_TIMER 0x20
#define TIMER_HZ 100

// Monotonic tick count since boot, at TIMER_HZ (100 Hz / 10 ms).
uint64_t timer_ticks(void);

// Monotonic nanoseconds since boot, from a calibrated rdtsc. Much finer
// than timer_ticks() -- the scheduler's virtual-time clock. Safe to
// call from any CPU (rdtsc is core-local but QEMU keeps them in sync,
// and the scheduler only needs per-CPU monotonicity).
uint64_t sched_clock_ns(void);

void timer_init(void);
// Arms THIS CPU's LAPIC timer. The BSP gets it from timer_init; every
// AP must call it for itself or it is never preempted.
void timer_init_this_cpu(void);
void timer_handler(void);

#endif
