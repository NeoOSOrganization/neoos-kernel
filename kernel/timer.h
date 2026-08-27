#ifndef NEOOS_TIMER_H
#define NEOOS_TIMER_H

#include <stdint.h>

#define VECTOR_TIMER 0x20
#define TIMER_HZ 100

// Monotonic tick count since boot, at TIMER_HZ.
uint64_t timer_ticks(void);

void timer_init(void);
void timer_handler(void);

#endif
