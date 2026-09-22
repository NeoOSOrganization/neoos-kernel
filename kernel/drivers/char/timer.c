#include "drivers/char/timer.h"
#include "drivers/char/pit.h"
#include "drivers/char/serial.h"
#include "arch/cpu.h"
#include "sync/waitq.h"
#include "arch/cpu_local.h"
#include "sched/proc.h"
#include "time/ktime.h"
#include "time/clockevent.h"
#include "time/hrtimer.h"

#define TICKS_PER_LOG 100          // 100Hz tick -> log once per second
#define HOUSE_NS      10000000ULL  // 10 ms: the housekeeping tick's period

// The timer vector is hrtimer_interrupt (kernel/time/hrtimer.c): every
// CPU's LAPIC timer is armed for the earliest of its hrtimers. The old
// 10 ms tick survives as one of them -- tick_fn below, periodic, per
// CPU -- until tickless idle removes it.

// Both clocks are derived from ktime (the TSC) now, so neither can
// drift the way the old armed-interval accumulator did.
uint64_t timer_ticks(void) { return ktime_get_ns() / TICK_NS; }
uint64_t sched_clock_ns(void) { return ktime_get_ns(); }

// System-wide CPU accounting, in timer ticks, summed across CPUs.
//
// Sampled by the tick because it runs regardless of what the CPU is
// doing: whatever thread the tick interrupted is what that CPU was
// running for the interval just elapsed. The idle thread is the marker
// -- a CPU running it had nothing else to do.
static volatile uint64_t cpu_busy_ticks, cpu_idle_ticks;

void cpu_usage_ticks(uint64_t *busy, uint64_t *idle) {
    if (busy) { *busy = cpu_busy_ticks; }
    if (idle) { *idle = cpu_idle_ticks; }
}

// The legacy 10 ms housekeeping tick, now just a periodic hrtimer per
// CPU. Its body is the old timer_handler minus the clock bookkeeping
// ktime made unnecessary and the preemption the slice hrtimer
// (sched_arm_slice_timer) now owns.
static struct hrtimer tick_timer[MAX_CPUS];

static enum hrtimer_restart tick_fn(struct hrtimer *t) {
    struct cpu *c = this_cpu();
    c->timer_ticks_local++;

    // Before a CPU enters the scheduler it has no current thread and is
    // neither busy nor idle in any meaningful sense; leave it out.
    if (c->current) {
        if (c->current == c->idle) {
            __atomic_fetch_add(&cpu_idle_ticks, 1, __ATOMIC_RELAXED);
        } else {
            __atomic_fetch_add(&cpu_busy_ticks, 1, __ATOMIC_RELAXED);
        }
    }

    // The BSP logs once per second of real (TSC) time, however the
    // interrupts fell.
    if (c == &cpus[0]) {
        static uint64_t last_tick;
        uint64_t now_tick = timer_ticks();
        while (last_tick < now_tick) {
            last_tick++;
            if (last_tick % TICKS_PER_LOG == 0) {
                serial_write_string("[timer] tick=");
                serial_write_hex64(last_tick);
                serial_write_string("\n");
            }
        }
    }

    hrtimer_forward_now(t, HOUSE_NS);
    return HRTIMER_RESTART;
}

void timer_handler(void) { hrtimer_interrupt(); }

void timer_init(void) {
    uint64_t tsc_per_10ms;
    uint32_t lapic_ticks_per_10ms = pit_calibrate_lapic_ticks_per_10ms(&tsc_per_10ms);
    ktime_calibrate(tsc_per_10ms);
    clockevent_init_bsp(lapic_ticks_per_10ms);
    serial_write_string("[timer] calibrated lapic ticks per 10ms=");
    serial_write_hex64(lapic_ticks_per_10ms);
    serial_write_string(" tsc per 10ms=");
    serial_write_hex64(tsc_per_10ms);
    serial_write_string("\n");

    timer_init_this_cpu();
}

// The LAPIC timer is a PER-CPU device: an AP must arm its own or it is
// never preempted. Calibration is not repeated -- the count is the same
// on every core.
void timer_init_this_cpu(void) {
    int idx = (int)(this_cpu() - &cpus[0]);
    hrtimer_cpu_init();
    clockevent_init_this_cpu();
    hrtimer_init(&tick_timer[idx], tick_fn);
    hrtimer_start(&tick_timer[idx], ktime_get_ns() + HOUSE_NS);
}
