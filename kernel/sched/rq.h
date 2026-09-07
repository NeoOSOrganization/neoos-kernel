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

// Defined in sched.c.
struct rq *cpu_rq(int cpu_index);
struct rq *this_rq(void);

void cfs_rq_init(struct cfs_rq *cfs);

// fair.c -- the class hooks sched.c calls (SCH-1 Task 1: FIFO via the
// tree; Task 3: real EEVDF).
void           fair_enqueue(struct rq *rq, struct thread *t);
struct thread *fair_pick(struct rq *rq);
struct thread *fair_steal(struct rq *rq);     // pop one for work-stealing
void           fair_put_prev(struct rq *rq, struct thread *prev);

#endif // NEOOS_SCHED_RQ_H
