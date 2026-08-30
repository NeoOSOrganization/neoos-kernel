#ifndef NEOOS_WAITQ_H
#define NEOOS_WAITQ_H

#include <stdint.h>
#include "spinlock_types.h"

// Deliberately does NOT include lock.h: lock.h includes THIS header
// (struct mutex embeds a struct waitq by value), so including it back
// would be circular. spinlock_types.h carries just the struct
// definition, which is all this header needs.
struct thread;

struct waitq {
    struct thread *head, *tail;
    // Protects head/tail. Before this existed the fields were covered
    // only by whichever guard the CALLER happened to hold -- a different
    // lock for different callers, which is no protection at all across
    // CPUs.
    struct spinlock lock;
};

void waitq_lock_selftest(void);

void waitq_init(struct waitq *q);

// Blocks the calling thread on `q`. If `release` is non-null it is
// unlocked before blocking and re-locked before returning. Returns 0
// on a normal wake, or -EINTR if the thread was killed while blocked.
//
// The classic lost-wakeup window between releasing `release` and
// switching away is closed by running with interrupts off, which is
// genuinely sufficient on one CPU: no waker can run. The SMP milestone
// replaces these internals with a lock handoff WITHOUT changing this
// signature, so no caller has to change.
int  waitq_sleep(struct waitq *q, struct spinlock *release);

// Like waitq_sleep, but also returns -ETIMEDOUT once timer_ticks()
// reaches `deadline`. nanosleep and futex(FUTEX_WAIT) with a timeout
// need exactly this, so it is paid for once here.
int  waitq_sleep_timeout(struct waitq *q, struct spinlock *release,
                         uint64_t deadline);

// Called once per timer tick to wake expired sleepers.
void waitq_timeout_tick(void);
void waitq_wake_one(struct waitq *q);
void waitq_wake_all(struct waitq *q);

// Removes `t` from whatever queue it is blocked on, leaving it
// dequeued but NOT ready. Used by thread_kill.
void waitq_remove(struct thread *t);

void waitq_selftest_start(void);

#endif
