// Per-CPU clock-event device: TSC-deadline when CPUID advertises it
// (KVM, real hardware), LAPIC one-shot count mode otherwise (QEMU TCG).
// See the hrtimer spec, section 1.
#include "time/clockevent.h"
#include "time/ktime.h"
#include "arch/cpu.h"
#include "arch/msr.h"
#include "drivers/irq/lapic.h"
#include "drivers/char/timer.h"
#include "drivers/char/serial.h"

#define MSR_IA32_TSC_DEADLINE 0x6E0

static int      use_tsc_deadline;
static uint32_t lapic_per_10ms;
static uint64_t min_ns;

void clockevent_init_bsp(uint32_t lapic_ticks_per_10ms) {
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    use_tsc_deadline = (c >> 24) & 1;
    lapic_per_10ms   = lapic_ticks_per_10ms;
    min_ns = use_tsc_deadline ? 2000 : 10000;
    serial_write_string(use_tsc_deadline ? "[hrtimer] clockevent: tsc-deadline\n"
                                         : "[hrtimer] clockevent: lapic-oneshot\n");
}

void clockevent_init_this_cpu(void) {
    if (use_tsc_deadline) { lapic_timer_set_tsc_deadline_mode(VECTOR_TIMER); }
}

uint64_t clockevent_min_ns(void) { return min_ns; }
const char *clockevent_name(void) { return use_tsc_deadline ? "tsc-deadline" : "lapic-oneshot"; }

void clockevent_program(uint64_t expires_ns) {
    uint64_t now = ktime_get_ns();
    if (expires_ns < now + min_ns) { expires_ns = now + min_ns; }
    if (use_tsc_deadline) {
        wrmsr(MSR_IA32_TSC_DEADLINE, ktime_ns_to_tsc(expires_ns));
        return;
    }
    // Count mode: 32-bit counter; longer intervals arm the maximum and
    // the caller simply re-programs when it fires early.
    uint64_t delta = expires_ns - now;
    uint64_t count = (uint64_t)((__uint128_t)delta * lapic_per_10ms / TICK_NS);
    if (count == 0) { count = 1; }
    if (count > 0xFFFFFFFFULL) { count = 0xFFFFFFFFULL; }
    lapic_timer_start_oneshot((uint32_t)count, VECTOR_TIMER);
}
