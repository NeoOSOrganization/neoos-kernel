#ifndef NEOOS_HRTIMER_H
#define NEOOS_HRTIMER_H
#include <stdint.h>
#include "lib/rbtree.h"

// High-resolution timers: absolute ktime_get_ns() expiries on a per-CPU
// rbtree, the clock-event device always armed for the earliest. See
// docs/superpowers/specs/2026-09-23-hrtimers-design.md section 2.
//
// Callbacks run in the timer interrupt with IRQs off and must never
// sleep. A callback re-arms ITSELF only by hrtimer_forward_now() (or by
// moving expires_ns forward) and returning HRTIMER_RESTART: calling
// hrtimer_start/hrtimer_cancel on its own timer would spin forever on
// its own RUNNING state.

enum hrtimer_restart { HRTIMER_NORESTART, HRTIMER_RESTART };

struct hrtimer {
    struct rb_node node;          // in the owning CPU's base
    uint64_t expires_ns;          // absolute, ktime_get_ns() scale
    enum hrtimer_restart (*fn)(struct hrtimer *);
    volatile uint8_t state;       // INACTIVE / QUEUED / RUNNING
    uint8_t cpu;                  // base it is queued on
};

void hrtimer_init(struct hrtimer *t, enum hrtimer_restart (*fn)(struct hrtimer *));
// Queues t on THIS CPU (re-keying it if it was already queued anywhere).
void hrtimer_start(struct hrtimer *t, uint64_t expires_ns);
// 1 dequeued, 0 was not queued, -1 its callback is running right now.
int  hrtimer_try_cancel(struct hrtimer *t);
// 1 if it was queued; waits out a running callback, so afterwards the
// callback is guaranteed not to be touching t.
int  hrtimer_cancel(struct hrtimer *t);
// Moves an expired t forward by whole intervals past now.
void hrtimer_forward_now(struct hrtimer *t, uint64_t interval_ns);
int  hrtimer_active(const struct hrtimer *t);
// For callbacks: schedule() once this interrupt's pass is done.
void hrtimer_request_resched(void);
// The timer vector's handler.
void hrtimer_interrupt(void);
// Per CPU, before its first timer is started.
void hrtimer_cpu_init(void);
void hrtimer_selftest(void);
#endif
