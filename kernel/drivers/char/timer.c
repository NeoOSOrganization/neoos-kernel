#include "drivers/char/timer.h"
#include "drivers/char/pit.h"
#include "drivers/char/serial.h"
#include "arch/cpu.h"
#include "arch/cpu_local.h"
#include "sched/proc.h"
#include "time/ktime.h"
#include "time/clockevent.h"
#include "time/hrtimer.h"
#include "smp/smp.h"

// The timer vector is hrtimer_interrupt (kernel/time/hrtimer.c): every
// CPU's LAPIC timer is armed for the earliest of its hrtimers, and for
// nothing else. There is no periodic tick. What the 10 ms tick used to
// do now lives where it belongs (spec section 5):
//   wall clock           -- derived from the TSC (ktime)
//   timed sleeps         -- each thread's sleep_timer
//   slice preemption     -- each CPU's sched_timer (sched.c)
//   TCP/ARP timeouts     -- the timer wheel
//   busy/idle accounting -- nanoseconds, charged at context switch
//   the once-a-second log -- log_timer below, BSP only
// So an idle CPU sleeps in hlt until its next real timer or an IPI.

// Both clocks are derived from ktime (the TSC) now, so neither can
// drift the way the old armed-interval accumulator did.
uint64_t timer_ticks(void) { return ktime_get_ns() / TICK_NS; }
uint64_t sched_clock_ns(void) { return ktime_get_ns(); }

// System-wide CPU accounting, in 10 ms units summed across CPUs (the
// /proc/stat unit). schedule() charges each CPU's busy/idle ns at every
// switch (timer_account_switch); the stretch in progress is added here,
// so a CPU that has been idle for a minute without switching still
// reports that minute. The idle thread is the marker -- a CPU running
// it had nothing else to do.
void cpu_usage_ticks(uint64_t *busy, uint64_t *idle) {
    uint64_t b = 0, i = 0;
    int online = smp_online_count();
    for (int k = 0; k < online; k++) {
        uint64_t bk, ik;
        cpu_usage_ticks_one(k, &bk, &ik);
        b += bk; i += ik;
    }
    if (busy) { *busy = b; }
    if (idle) { *idle = i; }
}

void cpu_usage_ticks_one(int cpu, uint64_t *busy, uint64_t *idle) {
    uint64_t now = ktime_get_ns(), b = 0, i = 0;
    for (int k = cpu; k == cpu; k++) {
        struct cpu *c = &cpus[k];
        b += __atomic_load_n(&c->busy_ns, __ATOMIC_RELAXED);
        i += __atomic_load_n(&c->idle_ns, __ATOMIC_RELAXED);
        uint64_t since = __atomic_load_n(&c->acct_since, __ATOMIC_RELAXED);
        struct thread *cur = c->current;
        if (cur && since && now > since) {
            if (cur == c->idle) { i += now - since; } else { b += now - since; }
        }
    }
    if (busy) { *busy = b / TICK_NS; }
    if (idle) { *idle = i / TICK_NS; }
}

// Called by schedule() on THIS CPU just before it switches away from
// prev (0 on the very first switch out of a bootstrap stack).
void timer_account_switch(struct thread *prev) {
    struct cpu *c = this_cpu();
    uint64_t now = ktime_get_ns();
    if (prev && c->acct_since && now > c->acct_since) {
        uint64_t d = now - c->acct_since;
        if (prev == c->idle) { __atomic_fetch_add(&c->idle_ns, d, __ATOMIC_RELAXED); }
        else                 { __atomic_fetch_add(&c->busy_ns, d, __ATOMIC_RELAXED); }
    }
    __atomic_store_n(&c->acct_since, now, __ATOMIC_RELAXED);
}

// The BSP's once-a-second log line, same format as when a 100 Hz tick
// printed it.
static struct hrtimer log_timer;
static enum hrtimer_restart log_fn(struct hrtimer *t) {
    serial_write_string("[timer] tick=");
    serial_write_hex64(timer_ticks());
    serial_write_string("\n");
    hrtimer_forward_now(t, NSEC_PER_SEC);
    return HRTIMER_RESTART;
}

// One interrupt per CPU shortly after bring-up, so every CPU provably
// takes a timer interrupt ([smp] local timer selftest) even if it then
// has nothing to do for the rest of the boot.
static struct hrtimer hello_timer[MAX_CPUS];
static enum hrtimer_restart hello_fn(struct hrtimer *t) { (void)t; return HRTIMER_NORESTART; }

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
    hrtimer_init(&hello_timer[idx], hello_fn);
    hrtimer_start(&hello_timer[idx], ktime_after_ns(1000000));   // 1 ms
    if (idx == 0) {
        hrtimer_init(&log_timer, log_fn);
        hrtimer_start(&log_timer, ktime_after_ns(NSEC_PER_SEC));
    }
}
