// kernel/sched/sched_entity.h -- the schedulable unit.
//
// Every thread carries one struct sched_entity. It holds the
// fair-class (EEVDF) bookkeeping and the rbtree node that links the
// thread into its CPU's cfs_rq. Group scheduling (SCH-5) will add one
// sched_entity per task_group per CPU; this header is the shared
// vocabulary.
//
// Spec: docs/superpowers/specs/2026-09-07-advanced-scheduler-design.md
// (SCH-1). Plan: docs/superpowers/plans/2026-09-07-sch1-eevdf-core.md.

#ifndef NEOOS_SCHED_ENTITY_H
#define NEOOS_SCHED_ENTITY_H

#include <stdint.h>
#include "lib/rbtree.h"

// SCHED_* policy numbers -- Linux's values (userland-visible via
// sched_setscheduler / sched_setattr).
#define SCHED_NORMAL    0
#define SCHED_FIFO      1
#define SCHED_RR        2
#define SCHED_BATCH     3
#define SCHED_IDLE      5
#define SCHED_DEADLINE  6

// Weight of nice 0. Virtual time advances at real_time * NICE_0_WEIGHT
// / se->load.weight, so a nice-0 task's vruntime tracks real time 1:1.
#define NICE_0_WEIGHT   1024

// SCHED_IDLE's weight -- lower than nice 19 (15), so an idle-policy
// task yields to everything.
#define WEIGHT_IDLEPRIO 3

// The default per-task request slice (the EEVDF "how long do I want to
// run before the scheduler reconsiders" knob), in nanoseconds.
// ~0.7 ms. Overridable per task via sched_setattr(sched_runtime).
#define SCHED_BASE_SLICE_NS 700000ULL

struct load_weight {
    uint64_t weight;      // from sched_prio_to_weight[nice + 20]
    uint32_t inv_weight;  // 2^32 / weight  (sched_prio_to_wmult), for the
                          // divide-free vruntime scaling in the hot path
};

struct sched_entity {
    struct rb_node     run_node;      // link in cfs_rq->tasks_timeline
    struct load_weight load;

    uint64_t vruntime;               // wrap-safe virtual time
    int64_t  vlag;                   // V - vruntime at last dequeue; a
                                     // just-woken task is re-placed with
                                     // this (decayed) lag preserved
    uint64_t slice;                  // request slice, ns (0 => base)
    uint64_t deadline;               // vruntime + slice/weight (EEVDF vd)
    uint64_t min_vruntime;           // subtree-min vruntime (rbtree augment)

    uint8_t  on_rq;                  // linked into a cfs_rq right now
    int      policy;                 // SCHED_NORMAL / BATCH / IDLE
    int      nice;                   // [-20,19]; the source of truth for
                                     // load.weight, re-applied at every
                                     // enqueue_entity (set_load_weight)
    uint8_t  batch_hint;             // SCHED_BATCH: skip wake-preemption

    uint64_t exec_start;             // rq->clock_task at last update_curr
    uint64_t sum_exec_runtime;       // total ns this entity has run
    uint64_t prev_sum_exec_runtime;  // snapshot at pick, for slice accounting

    // SCH-1 Task 1 only: a monotonic key that makes the tree behave as
    // an insertion-ordered FIFO until Task 3 switches the key to
    // `deadline`. Removed in Task 3.
    uint64_t fifo_key;
};

extern const int      sched_prio_to_weight[40];
extern const uint32_t sched_prio_to_wmult[40];

// nice in [-20, 19] -> table index [0, 39], clamped.
static inline int nice_to_index(int nice) {
    if (nice < -20) { nice = -20; }
    if (nice >  19) { nice =  19; }
    return nice + 20;
}

static inline void set_load_weight(struct sched_entity *se, int nice) {
    if (se->policy == SCHED_IDLE) {
        se->load.weight     = WEIGHT_IDLEPRIO;
        se->load.inv_weight = (uint32_t)((1ULL << 32) / WEIGHT_IDLEPRIO);
        return;
    }
    int idx = nice_to_index(nice);
    se->load.weight     = (uint64_t)sched_prio_to_weight[idx];
    se->load.inv_weight = sched_prio_to_wmult[idx];
}

// Wrap-safe "is a before b" for the 64-bit virtual clock. Never
// compare vruntime/deadline with a plain `<`.
static inline int vtime_before(uint64_t a, uint64_t b) {
    return (int64_t)(a - b) < 0;
}
static inline uint64_t vtime_min(uint64_t a, uint64_t b) {
    return vtime_before(a, b) ? a : b;
}

// se->slice, resolving 0 to the base.
static inline uint64_t slice_of(const struct sched_entity *se) {
    return se->slice ? se->slice : SCHED_BASE_SLICE_NS;
}

// delta_exec (ns) scaled into virtual time for `se`'s weight:
//   vdelta = delta_exec * NICE_0_WEIGHT / weight
//          = delta_exec * NICE_0_WEIGHT * inv_weight >> 32
// For a nice-0 task (weight 1024, inv 4194304) this is the identity.
static inline uint64_t calc_delta_fair(uint64_t delta_exec,
                                       const struct sched_entity *se) {
    if (se->load.weight == NICE_0_WEIGHT) { return delta_exec; }
    // delta_exec is small (a slice, << 2^40); NICE_0_WEIGHT is 2^10;
    // the product fits in 64 bits comfortably for realistic slices.
    __uint128_t p = (__uint128_t)delta_exec * NICE_0_WEIGHT;
    p = (p * se->load.inv_weight) >> 32;
    return (uint64_t)p;
}

#endif // NEOOS_SCHED_ENTITY_H
