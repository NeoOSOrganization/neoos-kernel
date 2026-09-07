#include "drivers/char/timer.h"
#include "drivers/char/pit.h"
#include "drivers/irq/lapic.h"
#include "drivers/char/serial.h"
#include "arch/cpu.h"
#include "sync/waitq.h"
#include "arch/cpu_local.h"
#include "sched/proc.h"
#include "sched/rq.h"

#define TICKS_PER_LOG 100          // 100Hz wall clock -> log once per second
#define HOUSE_NS      10000000ULL  // 10 ms: wall-clock cadence + max one-shot

// SCH-1 Task 4: the LAPIC timer now runs in ONE-SHOT mode. timer_handler
// re-arms it on every interrupt for the running task's remaining slice
// (fair_slice_remaining_ns), capped at HOUSE_NS so the wall clock never
// goes more than 10 ms without an update. Preemption is therefore
// event-driven -- a 0.7 ms base slice actually means 0.7 ms, not the old
// fixed 50 ms -- while an idle CPU still only wakes at 100 Hz.
//
// DEBUG_HZ (kernel build knob) is now a no-op: preemption granularity is
// the per-task slice, tunable at runtime via sched_setattr.

static volatile uint64_t tick_count = 0;
static uint32_t lapic_ticks_per_10ms = 0;

// rdtsc-based nanosecond clock calibration (BSP, at timer_init).
static uint64_t g_tsc_base = 0;
static uint64_t g_tsc_per_10ms = 0;

// BSP-only: real nanoseconds accumulated toward the next 10 ms wall tick.
static uint64_t bsp_ns_accum = 0;

uint64_t timer_ticks(void) { return tick_count; }

uint64_t sched_clock_ns(void) {
    uint64_t per = g_tsc_per_10ms;
    if (per == 0) { return 0; }                 // pre-calibration
    uint64_t d = rdtsc() - g_tsc_base;
    return (uint64_t)((__uint128_t)d * HOUSE_NS / per);
}

static uint32_t ns_to_lapic_count(uint64_t ns) {
    if (lapic_ticks_per_10ms == 0) { return 1; }
    uint64_t c = (uint64_t)((__uint128_t)ns * lapic_ticks_per_10ms / HOUSE_NS);
    uint32_t lo = lapic_ticks_per_10ms / 20;    // ~500 us floor: interrupt-storm guard
    if (lo == 0) { lo = 1; }
    if (c < lo) { c = lo; }
    if (c > lapic_ticks_per_10ms) { c = lapic_ticks_per_10ms; }
    return (uint32_t)c;
}

// EVERY CPU takes this from its own LAPIC one-shot. Only the BSP owns
// the shared wall clock: tick_count++ is a read-modify-write, so four
// CPUs racing on it would lose updates and run time fast.
void timer_handler(void) {
    struct cpu *c = this_cpu();
    c->timer_ticks_local++;

    // The one-shot always runs to completion, so the interval we armed
    // last time is exactly the real time that has elapsed.
    uint64_t elapsed = c->timer_armed_ns ? c->timer_armed_ns : HOUSE_NS;

    if (c == &cpus[0]) {
        bsp_ns_accum += elapsed;
        while (bsp_ns_accum >= HOUSE_NS) {
            bsp_ns_accum -= HOUSE_NS;
            tick_count++;
            if (tick_count % TICKS_PER_LOG == 0) {
                serial_write_string("[timer] tick=");
                serial_write_hex64(tick_count);
                serial_write_string("\n");
            }
            // Wake anything whose timed sleep has expired. A scan per
            // tick is cheaper than a heap at NeoOS's thread counts.
            waitq_timeout_tick();
        }
    }

    // Never preempt a CPU that has not yet entered the scheduler: before
    // its first schedule() it is still on a BOOTSTRAP stack (kmain on
    // the BSP, ap_main on an AP) with no thread to save that context
    // into. tlb_shootdown enables interrupts while waiting for acks, and
    // a tick landing in that window would strand the BSP mid-kmain.
    uint64_t next_ns = HOUSE_NS;
    if (c->current) {
        if (sched_tick(&c->rq)) {
            schedule();
            c = this_cpu();   // current has changed; still this CPU
        }
        uint64_t rem = sched_slice_remaining_ns(&c->rq);
        if (rem < next_ns) { next_ns = rem; }
    }

    c->timer_armed_ns = next_ns;
    lapic_timer_start_oneshot(ns_to_lapic_count(next_ns), VECTOR_TIMER);
}

void timer_init(void) {
    lapic_ticks_per_10ms = pit_calibrate_lapic_ticks_per_10ms(&g_tsc_per_10ms);
    g_tsc_base = rdtsc();
    serial_write_string("[timer] calibrated lapic ticks per 10ms=");
    serial_write_hex64(lapic_ticks_per_10ms);
    serial_write_string(" tsc per 10ms=");
    serial_write_hex64(g_tsc_per_10ms);
    serial_write_string("\n");

    timer_init_this_cpu();
}

// The LAPIC timer is a PER-CPU device: an AP must arm its own or it is
// never preempted. Calibration is not repeated -- the count is the same
// on every core.
void timer_init_this_cpu(void) {
    this_cpu()->timer_armed_ns = HOUSE_NS;
    lapic_timer_start_oneshot(lapic_ticks_per_10ms, VECTOR_TIMER);
}
