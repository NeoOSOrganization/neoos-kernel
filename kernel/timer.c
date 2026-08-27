#include "timer.h"
#include "pit.h"
#include "lapic.h"
#include "serial.h"
#include "waitq.h"
#include "sched/proc.h"

#define TICKS_PER_LOG 100 // 100Hz timer -> log once per second
#define TIMESLICE_TICKS 5  // 100Hz timer, 5 ticks = 50ms per time slice

static volatile uint64_t tick_count = 0;
static uint32_t timeslice_remaining = TIMESLICE_TICKS;

uint64_t timer_ticks(void) { return tick_count; }

void timer_handler(void) {
    tick_count++;
    if (tick_count % TICKS_PER_LOG == 0) {
        serial_write_string("[timer] tick=");
        serial_write_hex64(tick_count);
        serial_write_string("\n");
    }

    // Wake anything whose timed sleep has expired. A scan per tick is
    // cheaper than a heap at NeoOS's thread counts, and much easier to
    // get right.
    waitq_timeout_tick();

    if (--timeslice_remaining == 0) {
        timeslice_remaining = TIMESLICE_TICKS;
        schedule();
    }
}

void timer_init(void) {
    // Calibrating over exactly 10ms and targeting 100Hz (10ms period)
    // means the calibrated tick count IS the periodic initial count.
    uint32_t ticks_per_10ms = pit_calibrate_lapic_ticks_per_10ms();
    serial_write_string("[timer] calibrated lapic ticks per 10ms=");
    serial_write_hex64(ticks_per_10ms);
    serial_write_string("\n");

    lapic_timer_start_periodic(ticks_per_10ms, VECTOR_TIMER);
}
