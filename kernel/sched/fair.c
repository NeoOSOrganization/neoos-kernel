// kernel/sched/fair.c -- the fair scheduling class.
//
// SCH-1 Task 1: the EEVDF structures exist but the tree is still an
// insertion-ordered FIFO (keyed by a per-rq monotonic counter), so
// scheduling behaviour is byte-for-byte the round-robin it was before.
// Tasks 2-4 replace the key with the EEVDF virtual deadline and make
// the pick eligibility-constrained.
//
// Locking: every function here runs with the target rq->lock already
// held by the sched.c caller -- matching how ready_push/ready_pop
// worked.

#include "sched/rq.h"
#include "sched/sched_entity.h"
#include "sched/proc.h"

void cfs_rq_init(struct cfs_rq *cfs) {
    cfs->tasks_timeline = RB_ROOT_CACHED;
    cfs->nr_running = 0;
    cfs->min_vruntime = 0;
    cfs->avg_vruntime = 0;
    cfs->avg_load = 0;
    cfs->curr = 0;
    cfs->fifo_seq = 0;
}

// se is embedded in struct thread; recover the container.
static inline struct thread *task_of(struct sched_entity *se) {
    return rb_entry(se, struct thread, se);
}

// SCH-1 Task 1 ordering: strictly by insertion order (fifo_key).
static inline int se_before(const struct sched_entity *a,
                            const struct sched_entity *b) {
    return a->fifo_key < b->fifo_key;
}

void fair_enqueue(struct rq *rq, struct thread *t) {
    struct cfs_rq *cfs = &rq->cfs;
    struct sched_entity *se = &t->se;

    se->fifo_key = ++cfs->fifo_seq;

    struct rb_node **link = &cfs->tasks_timeline.rb_root.rb_node;
    struct rb_node  *parent = 0;
    int leftmost = 1;
    while (*link) {
        parent = *link;
        struct sched_entity *pse = rb_entry(parent, struct sched_entity, run_node);
        if (se_before(se, pse)) {
            link = &parent->rb_left;
        } else {
            link = &parent->rb_right;
            leftmost = 0;
        }
    }
    rb_link_node(&se->run_node, parent, link);
    rb_insert_color_cached(&se->run_node, &cfs->tasks_timeline, leftmost);

    se->on_rq = 1;
    cfs->nr_running++;
}

// Remove and return the front entity's thread, or 0 if the rq is
// empty. Does NOT touch cfs->curr or the CPU's current pointer --
// sched.c owns those.
static struct thread *fair_pop_front(struct rq *rq) {
    struct cfs_rq *cfs = &rq->cfs;
    struct rb_node *n = rb_first_cached(&cfs->tasks_timeline);
    if (!n) { return 0; }

    struct sched_entity *se = rb_entry(n, struct sched_entity, run_node);
    rb_erase_cached(n, &cfs->tasks_timeline);
    se->on_rq = 0;
    cfs->nr_running--;
    return task_of(se);
}

struct thread *fair_pick(struct rq *rq) {
    struct thread *t = fair_pop_front(rq);
    if (t) { rq->cfs.curr = &t->se; }
    return t;
}

struct thread *fair_steal(struct rq *rq) {
    // A stolen thread is not "current" anywhere yet.
    return fair_pop_front(rq);
}

void fair_put_prev(struct rq *rq, struct thread *prev) {
    // The task that just lost the CPU goes back on the queue. Under
    // the FIFO key it lands at the back, exactly as ready_push did for
    // a preempted thread.
    rq->cfs.curr = 0;
    fair_enqueue(rq, prev);
}
