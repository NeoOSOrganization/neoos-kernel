// kernel/sched/rq.h -- the per-CPU runqueue.
//
// One struct rq per CPU, embedded in struct cpu (see cpu_local.h). It
// replaces the loose cpu.ready_* fields. SCH-1 fills in cfs (the EEVDF
// fair class); rt_rq / dl_rq join in later milestones.
//
// Spec: docs/superpowers/specs/2026-09-07-advanced-scheduler-design.md

#ifndef NEOOS_SCHED_RQ_H
#define NEOOS_SCHED_RQ_H

#include <stdint.h>
#include "sync/lock.h"
#include "lib/rbtree.h"

struct thread;
struct sched_entity;

struct cfs_rq {
    struct rb_root_cached tasks_timeline;   // SCH-1 T1: keyed by se->fifo_key
                                            // SCH-1 T3: keyed by se->deadline
    unsigned int          nr_running;

    // EEVDF virtual time (SCH-1 Task 2 onward; zero-init and unused
    // until then).
    uint64_t min_vruntime;       // V's integer floor; monotonic
    int64_t  avg_vruntime;       // Sum (v_i - min_vruntime) * w_i
    uint64_t avg_load;           // Sum w_i
    struct sched_entity *curr;   // the running entity, or 0

    // SCH-1 Task 1: the FIFO ordering key. Removed in Task 3.
    uint64_t fifo_seq;
};

struct rq {
    struct spinlock  lock;       // LOCK_RANK_RUNQUEUE (kept name for T1)
    struct cfs_rq    cfs;

    // Moved here from struct cpu; identical semantics -- the thread
    // this CPU switched away from but has not yet released. Consumed by
    // sched_post_switch().
    struct thread   *prev_pending;

    // Monotonic ns, set once per schedule()/tick (SCH-1 Task 3+).
    uint64_t clock;
    uint64_t clock_task;
};

// The struct rq that embeds this cfs_rq.
#define rq_of(cfsp) \
    ((struct rq *)((char *)(cfsp) - offsetof(struct rq, cfs)))

// Defined in sched.c.
struct rq *cpu_rq(int cpu_index);
struct rq *this_rq(void);

void cfs_rq_init(struct cfs_rq *cfs);

// fair.c -- the class hooks sched.c calls. All require rq->lock held
// and (for fair_pick) rq->clock_task current.
void           fair_enqueue(struct rq *rq, struct thread *t);        // wake / new
struct thread *fair_pick(struct rq *rq);                            // pick + set curr
void           fair_block_current(struct rq *rq, struct thread *prev);   // prev blocked
void           fair_requeue_preempted(struct rq *rq, struct thread *prev); // prev preempted
struct thread *fair_steal(struct rq *rq);                           // pop one to migrate
void           fair_accept_stolen(struct rq *rq, struct thread *t); // migrated-in

// Set rq->clock / rq->clock_task from the monotonic ns source.
void rq_clock_update(struct rq *rq);

// Called from timer_handler on every tick. Takes rq->lock, charges the
// running task, and returns 1 if it should be preempted now (slice
// spent, or a more-eligible task is waiting past the anti-thrash
// floor). The caller then invokes schedule().
int fair_entity_tick(struct rq *rq);          // fair.c, rq->lock held
int sched_tick(struct rq *rq);                // sched.c, takes rq->lock

// Nanoseconds of slice the running task has left before it must be
// preempted, for arming the one-shot timer. HOUSEKEEPING_NS if idle.
uint64_t fair_slice_remaining_ns(struct rq *rq);   // fair.c, rq->lock held
uint64_t sched_slice_remaining_ns(struct rq *rq);  // sched.c, takes rq->lock

// Scheduler ABI helpers (SCH-1 Task 5). fair_* need rq->lock held.
void fair_reweight_current(struct rq *rq, int nice, int policy, uint64_t slice_ns);
void fair_yield_current(struct rq *rq);

struct thread;
// Apply a nice/policy/slice change to `t`. If `t` is the running task on
// this CPU it takes effect immediately; otherwise it is recorded on the
// entity and applied at its next enqueue. slice_ns == (uint64_t)-1 means
// "leave slice unchanged". Takes the rq lock.
void sched_apply_attr(struct thread *t, int nice, int policy, uint64_t slice_ns);
// Real sched_yield for the calling thread. Takes the rq lock, then
// schedule()s.
void sched_do_yield(void);

// Boot selftest: the EEVDF virtual-time arithmetic.
void eevdf_selftest(void);

#endif // NEOOS_SCHED_RQ_H
