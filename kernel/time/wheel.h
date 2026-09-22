#ifndef NEOOS_WHEEL_H
#define NEOOS_WHEEL_H
#include <stdint.h>

// A hierarchical timer wheel for coarse kernel timeouts (TCP, ARP):
// O(1) arm and cancel, fires within one bucket of its jiffy, never
// early. Callbacks run in the ktimerd kernel thread -- they may take
// locks and transmit, but must not sleep for long. See
// docs/superpowers/specs/2026-09-23-hrtimers-design.md section 4.

#define JIFFY_NS 1000000ULL                 // 1 ms: jiffies run at 1000 Hz

struct timer_list {
    struct timer_list *next, **pprev;       // pprev != 0 <=> pending
    uint64_t expires;                       // jiffies
    void (*fn)(struct timer_list *);
};

uint64_t jiffies(void);                     // ktime_get_ns() / JIFFY_NS
uint64_t msecs_to_jiffies(uint64_t ms);
void timer_setup(struct timer_list *t, void (*fn)(struct timer_list *));
// Arm, or re-arm at a new expiry. Safe from any context that may take
// a rank-16 lock, including the timer's own callback.
void mod_timer(struct timer_list *t, uint64_t expires);
// Arm if idle; if pending, only ever move the expiry EARLIER.
void timer_reduce(struct timer_list *t, uint64_t expires);
int  timer_pending(const struct timer_list *t);
int  del_timer(struct timer_list *t);       // 1 if it was pending
// del_timer, then wait out a callback already running. Never call it
// on a timer from that timer's own callback.
int  del_timer_sync(struct timer_list *t);
void wheel_init(void);                      // before the first mod_timer; spawns ktimerd
void wheel_selftest(void);                  // synchronous, simulated clock: "[wheel] selftest passed"
void wheel_selftest_start(void);            // thread, real ktimerd: "[wheel] ktimerd selftest passed"
#endif
