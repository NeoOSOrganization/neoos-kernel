#include "timer.h"
#include "pit.h"
#include "lapic.h"
#include "serial.h"
#include "waitq.h"
#include "cpu_local.h"
#include "sched/proc.h"

#define TICKS_PER_LOG 100 // 100Hz timer -> log once per second
#define TIMESLICE_TICKS 5  // 100Hz timer, 5 ticks = 50ms per time slice

static volatile uint64_t tick_count = 0;
static uint32_t lapic_ticks_per_10ms;

uint64_t timer_ticks(void) { return tick_count; }

// EVERY CPU takes this interrupt now, from its own LAPIC timer. Only the
// wall clock is shared, and only the BSP advances it: `tick_count++` is
// a read-modify-write, so four CPUs doing it would both lose updates and
// run the clock at four times real speed. The time slice, by contrast,
// is genuinely per-CPU -- it is about the thread THIS CPU is running.
void timer_handler(void) {
    struct cpu *c = this_cpu();
    c->timer_ticks_local++;

    if (c == &cpus[0]) {
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
    }

    if (--c->timeslice_remaining == 0) {
        c->timeslice_remaining = TIMESLICE_TICKS;
        schedule();
    }
}

void timer_init(void) {
    // Calibrating over exactly 10ms and targeting 100Hz (10ms period)
    // means the calibrated tick count IS the periodic initial count.
    lapic_ticks_per_10ms = pit_calibrate_lapic_ticks_per_10ms();
    serial_write_string("[timer] calibrated lapic ticks per 10ms=");
    serial_write_hex64(lapic_ticks_per_10ms);
    serial_write_string("\n");

    timer_init_this_cpu();
}

// The LAPIC timer is a PER-CPU device: programming it on the BSP starts
// the BSP's timer and nobody else's. Until every CPU armed its own, an
// AP could not preempt anything -- a compute-bound thread that reached
// one ran to completion with no time slicing at all, which is why work
// stealing had nothing safe to steal to. The PIT calibration is not
// repeated; the count is the same on every core.
void timer_init_this_cpu(void) {
    this_cpu()->timeslice_remaining = TIMESLICE_TICKS;
    lapic_timer_start_periodic(lapic_ticks_per_10ms, VECTOR_TIMER);
}
