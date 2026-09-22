#ifndef NEOOS_CLOCKEVENT_H
#define NEOOS_CLOCKEVENT_H
#include <stdint.h>

// The per-CPU timer interrupt source: TSC-deadline when the CPU has it,
// LAPIC one-shot count mode otherwise. Chosen once at boot.
void clockevent_init_bsp(uint32_t lapic_ticks_per_10ms);
// Puts THIS CPU's LAPIC timer in the chosen mode. Every CPU calls it.
void clockevent_init_this_cpu(void);
// Arms THIS CPU for an absolute ktime expiry; one closer than the
// device minimum is armed at now + minimum instead.
void clockevent_program(uint64_t expires_ns);
uint64_t clockevent_min_ns(void);
const char *clockevent_name(void);
#endif
