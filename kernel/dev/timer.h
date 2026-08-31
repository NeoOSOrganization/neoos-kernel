#ifndef NEOOS_TIMER_H
#define NEOOS_TIMER_H

#include <stdint.h>

#define VECTOR_TIMER 0x20
#define TIMER_HZ 100

// Monotonic tick count since boot, at TIMER_HZ.
uint64_t timer_ticks(void);

void timer_init(void);
// Arms THIS CPU's LAPIC timer. The BSP gets it from timer_init; every
// AP must call it for itself or it is never preempted.
void timer_init_this_cpu(void);
void timer_handler(void);

#endif
