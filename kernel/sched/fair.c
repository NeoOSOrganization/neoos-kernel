// kernel/sched/fair.c -- the fair scheduling class (EEVDF).
//
// Earliest Eligible Virtual Deadline First. Virtual time V advances at
// (Sum of runnable weights)^-1 per unit real time. Each entity has a
// lag = V - v_i; it is "eligible" when lag >= 0 (v_i <= V). Among
// eligible entities we run the one with the smallest virtual deadline
// vd_i = v_i + slice_i/w_i. The cfs_rq tree is a BST on `deadline`,
// augmented with the subtree-minimum vruntime so the eligibility-
// constrained earliest-deadline search is O(log n).
//
// Spec: docs/superpowers/specs/2026-09-07-advanced-scheduler-design.md
// Plan: docs/superpowers/plans/2026-09-07-sch1-eevdf-core.md
//
// Locking: every function here runs with the target rq->lock already
// held by the sched.c caller.

#include "sched/rq.h"
#include "sched/sched_entity.h"
#include "sched/proc.h"

// ---- container recovery -------------------------------------------------

static inline struct thread *task_of(struct sched_entity *se) {
    return rb_entry(se, struct thread, se);
}
static inline struct sched_entity *se_of_node(struct rb_node *n) {
    return rb_entry(n, struct sched_entity, run_node);
}

// scale_load_down: Linux keeps weights *1024 internally for group
// scheduling fixed-point; NeoOS has no group scheduling yet, so the
// weight IS the scaled-down value. Left as a named no-op so the EEVDF
// math reads like the reference.
static inline uint64_t sld(uint64_t w) { return w; }

// ---- virtual-time sums ------------------------------------------------

// v_i measured relative to the rq's min_vruntime, to keep the running
// sums small and wrap-safe.
static inline int64_t entity_key(struct cfs_rq *cfs, struct sched_entity *se) {
    return (int64_t)(se->vruntime - cfs->min_vruntime);
}

// Add / remove an entity's contribution to the weighted-vruntime sums.
// Called from __enqueue_entity / __dequeue_entity (the actual tree
// insert / remove) -- NOT when curr is picked, so `curr` is NOT in
// these sums and avg_vruntime()/entity_eligible() add it on the fly.
static void avg_vruntime_add(struct cfs_rq *cfs, struct sched_entity *se) {
    uint64_t w = sld(se->load.weight);
    cfs->avg_vruntime += entity_key(cfs, se) * (int64_t)w;
    cfs->avg_load     += w;
}
static void avg_vruntime_sub(struct cfs_rq *cfs, struct sched_entity *se) {
    uint64_t w = sld(se->load.weight);
    cfs->avg_vruntime -= entity_key(cfs, se) * (int64_t)w;
    cfs->avg_load     -= w;
}
// min_vruntime moved forward by `delta`: each stored term (v_i - min)*w_i
// loses delta*w_i, so the whole sum loses avg_load*delta.
static void avg_vruntime_shift(struct cfs_rq *cfs, int64_t delta) {
    cfs->avg_vruntime -= (int64_t)cfs->avg_load * delta;
}

// V, the weighted-average virtual time (integer floor).
static uint64_t avg_vruntime(struct cfs_rq *cfs) {
    struct sched_entity *curr = cfs->curr;
    int64_t  avg  = cfs->avg_vruntime;
    int64_t  load = (int64_t)cfs->avg_load;

    if (curr && curr->on_rq) {
        uint64_t w = sld(curr->load.weight);
        avg  += entity_key(cfs, curr) * (int64_t)w;
        load += (int64_t)w;
    }
    if (load <= 0) { return cfs->min_vruntime; }

    // avg may be negative; do a signed division that rounds toward
    // -inf so the result is the true weighted mean floor.
    if (avg < 0) { avg -= (load - 1); }
    return cfs->min_vruntime + (uint64_t)(avg / load);
}

// vruntime <= V, without dividing. `key` is a raw vruntime value
// (used both for a real entity and for a subtree's min_vruntime).
static int vruntime_eligible(struct cfs_rq *cfs, uint64_t vruntime) {
    struct sched_entity *curr = cfs->curr;
    int64_t  avg  = cfs->avg_vruntime;
    int64_t  load = (int64_t)cfs->avg_load;

    if (curr && curr->on_rq) {
        uint64_t w = sld(curr->load.weight);
        avg  += entity_key(cfs, curr) * (int64_t)w;
        load += (int64_t)w;
    }
    // key = vruntime - min_vruntime ; eligible iff avg >= key*load
    int64_t key = (int64_t)(vruntime - cfs->min_vruntime);
    return avg >= key * load;
}
static int entity_eligible(struct cfs_rq *cfs, struct sched_entity *se) {
    return vruntime_eligible(cfs, se->vruntime);
}

// ---- min_vruntime augment (subtree-minimum vruntime) ------------------

static uint64_t node_min_vruntime(struct rb_node *n) {
    struct sched_entity *se = se_of_node(n);
    uint64_t m = se->vruntime;
    if (n->rb_left) {
        m = vtime_min(m, se_of_node(n->rb_left)->min_vruntime);
    }
    if (n->rb_right) {
        m = vtime_min(m, se_of_node(n->rb_right)->min_vruntime);
    }
    return m;
}
static void mv_propagate(struct rb_node *node, struct rb_node *stop) {
    while (node != stop) {
        struct sched_entity *se = se_of_node(node);
        uint64_t m = node_min_vruntime(node);
        if (se->min_vruntime == m) { break; }
        se->min_vruntime = m;
        node = rb_parent(node);
    }
}
static void mv_copy(struct rb_node *old, struct rb_node *newn) {
    se_of_node(newn)->min_vruntime = se_of_node(old)->min_vruntime;
}
static void mv_rotate(struct rb_node *old, struct rb_node *newn) {
    mv_copy(old, newn);
    se_of_node(old)->min_vruntime = node_min_vruntime(old);
}
static const struct rb_augment_callbacks min_vruntime_cb = {
    mv_propagate, mv_copy, mv_rotate,
};

// ---- min_vruntime (== V's integer floor, monotonic) ------------------

static struct sched_entity *pick_root_entity(struct cfs_rq *cfs) {
    struct rb_node *r = cfs->tasks_timeline.rb_root.rb_node;
    return r ? se_of_node(r) : 0;
}
static struct sched_entity *pick_first_entity(struct cfs_rq *cfs) {
    struct rb_node *n = rb_first_cached(&cfs->tasks_timeline);
    return n ? se_of_node(n) : 0;
}

static void update_min_vruntime(struct cfs_rq *cfs) {
    struct sched_entity *root = pick_root_entity(cfs);
    struct sched_entity *curr = cfs->curr;
    uint64_t v = cfs->min_vruntime;

    if (curr && !curr->on_rq) { curr = 0; }

    if (curr && root) {
        v = vtime_min(curr->vruntime, root->min_vruntime);
    } else if (curr) {
        v = curr->vruntime;
    } else if (root) {
        v = root->min_vruntime;
    }

    // Never move backwards.
    int64_t delta = (int64_t)(v - cfs->min_vruntime);
    if (delta > 0) {
        avg_vruntime_shift(cfs, delta);
        cfs->min_vruntime = v;
    }
}

// ---- tree insert / remove ------------------------------------------------

// Ordered by virtual deadline (wrap-safe).
static int deadline_less(struct sched_entity *a, struct sched_entity *b) {
    return vtime_before(a->deadline, b->deadline);
}

static void __enqueue_entity(struct cfs_rq *cfs, struct sched_entity *se) {
    avg_vruntime_add(cfs, se);
    se->min_vruntime = se->vruntime;   // fresh leaf: own value (the contract)

    struct rb_node **link = &cfs->tasks_timeline.rb_root.rb_node;
    struct rb_node  *parent = 0;
    int leftmost = 1;
    while (*link) {
        parent = *link;
        if (deadline_less(se, se_of_node(parent))) {
            link = &parent->rb_left;
        } else {
            link = &parent->rb_right;
            leftmost = 0;
        }
    }
    rb_link_node(&se->run_node, parent, link);
    rb_insert_augmented_cached(&se->run_node, &cfs->tasks_timeline, leftmost,
                               &min_vruntime_cb);
}

static void __dequeue_entity(struct cfs_rq *cfs, struct sched_entity *se) {
    rb_erase_augmented_cached(&se->run_node, &cfs->tasks_timeline, &min_vruntime_cb);
    avg_vruntime_sub(cfs, se);
}

// ---- exec accounting / deadlines / placement -------------------------

static void update_deadline(struct cfs_rq *cfs, struct sched_entity *se) {
    (void)cfs;
    if ((int64_t)(se->vruntime - se->deadline) < 0) {
        return;   // current slice not spent yet
    }
    se->slice    = slice_of(se);
    se->deadline = se->vruntime + calc_delta_fair(se->slice, se);
}

// Charge `curr` for the real time it has run since exec_start (now =
// rq->clock_task) and advance its virtual time.
static void update_curr(struct cfs_rq *cfs) {
    struct sched_entity *curr = cfs->curr;
    if (!curr) { return; }

    uint64_t now = rq_of(cfs)->clock_task;
    if (now <= curr->exec_start) {
        curr->exec_start = now;
        return;
    }
    uint64_t delta = now - curr->exec_start;
    curr->exec_start        = now;
    curr->sum_exec_runtime += delta;
    curr->vruntime         += calc_delta_fair(delta, curr);

    update_deadline(cfs, curr);
    update_min_vruntime(cfs);
}

static void update_entity_lag(struct cfs_rq *cfs, struct sched_entity *se) {
    int64_t  lag   = (int64_t)(avg_vruntime(cfs) - se->vruntime);
    uint64_t twoslc = 2 * slice_of(se);
    uint64_t base   = twoslc > 10000000ULL ? twoslc : 10000000ULL;   // >= 1 tick
    int64_t  limit  = (int64_t)calc_delta_fair(base, se);
    if (lag >  limit) { lag =  limit; }
    if (lag < -limit) { lag = -limit; }
    se->vlag = lag;
}

// Set se->vruntime and se->deadline for a task entering the rq.
// `initial` != 0 for a brand-new task (halved first slice).
static void place_entity(struct cfs_rq *cfs, struct sched_entity *se, int initial) {
    uint64_t V      = avg_vruntime(cfs);
    uint64_t vslice = calc_delta_fair(slice_of(se), se);
    int64_t  lag    = 0;

    if (cfs->nr_running && se->vlag != 0) {
        // Clamp the carried lag so a long sleep can't hoard time.
        uint64_t twoslc = 2 * slice_of(se);
        uint64_t base   = twoslc > 10000000ULL ? twoslc : 10000000ULL;
        int64_t  limit  = (int64_t)calc_delta_fair(base, se);
        int64_t  l = se->vlag;
        if (l >  limit) { l =  limit; }
        if (l < -limit) { l = -limit; }

        // Compensate for the V-shift the insertion causes, so the
        // entity's post-insertion lag is `l`:
        //   place with l' = l * (W + w_i) / W
        int64_t W = (int64_t)cfs->avg_load;
        struct sched_entity *curr = cfs->curr;
        if (curr && curr->on_rq) { W += (int64_t)sld(curr->load.weight); }
        if (W < 1) { W = 1; }
        lag = (l * (W + (int64_t)sld(se->load.weight))) / W;
    }

    se->vruntime = V - (uint64_t)lag;
    if (initial) { vslice /= 2; }
    se->deadline = se->vruntime + vslice;
    se->vlag = 0;
}

// ---- pick ------------------------------------------------------------

static struct sched_entity *pick_eevdf(struct cfs_rq *cfs) {
    struct rb_node *node = cfs->tasks_timeline.rb_root.rb_node;
    struct sched_entity *curr = cfs->curr;
    struct sched_entity *first = pick_first_entity(cfs);
    struct sched_entity *best  = 0;

    if (cfs->nr_running == 1) {
        return (curr && curr->on_rq) ? curr : first;
    }
    if (curr && (!curr->on_rq || !entity_eligible(cfs, curr))) {
        curr = 0;
    }

    // Fast path: the earliest-deadline entity overall is eligible.
    if (first && entity_eligible(cfs, first)) {
        best = first;
        goto done;
    }

    while (node) {
        struct rb_node *left = node->rb_left;
        // An eligible entity in the left subtree always has an earlier
        // (or equal) deadline than this node -- prefer it.
        if (left && vruntime_eligible(cfs, se_of_node(left)->min_vruntime)) {
            node = left;
            continue;
        }
        struct sched_entity *se = se_of_node(node);
        if (entity_eligible(cfs, se)) {
            best = se;
            break;
        }
        node = node->rb_right;
    }

done:
    if (!best || (curr && vtime_before(curr->deadline, best->deadline))) {
        best = curr;
    }
    if (!best) { best = first; }   // nothing eligible: take the min-vruntime one
    return best;
}


// ---- class hooks (called by sched.c with rq->lock held) --------------
//
// on_rq semantics: 1 while the thread is RUNNABLE -- whether it sits in
// the tree or is cfs->curr. Cleared only when it blocks/exits.
// nr_running counts every runnable entity (tree + curr). The FIELD sums
// (avg_vruntime/avg_load) cover IN-TREE entities only; avg_vruntime()
// adds curr's live contribution.

// A thread becomes runnable (wake, or brand-new).
static void enqueue_entity(struct cfs_rq *cfs, struct sched_entity *se) {
    update_curr(cfs);
    place_entity(cfs, se, se->sum_exec_runtime == 0);
    __enqueue_entity(cfs, se);
    se->on_rq = 1;
    cfs->nr_running++;
}

// A thread stops being runnable (block, exit, or migrate away).
static void dequeue_entity(struct cfs_rq *cfs, struct sched_entity *se) {
    update_curr(cfs);
    update_entity_lag(cfs, se);
    if (se == cfs->curr) {
        cfs->curr = 0;                 // was running, not in the tree
    } else {
        __dequeue_entity(cfs, se);     // in the tree
    }
    se->on_rq = 0;
    cfs->nr_running--;
    update_min_vruntime(cfs);
}

// A tree entity is chosen to run: pull it out of the tree, make it curr.
static void set_next_entity(struct cfs_rq *cfs, struct sched_entity *se) {
    if (se->on_rq && se != cfs->curr) {
        __dequeue_entity(cfs, se);
    }
    se->prev_sum_exec_runtime = se->sum_exec_runtime;
    cfs->curr = se;
}

// A running entity is preempted: put it back in the tree (keeping its
// vruntime -- it lost the CPU, not its place).
static void put_prev_entity(struct cfs_rq *cfs, struct sched_entity *se) {
    update_curr(cfs);
    if (se->on_rq) {
        __enqueue_entity(cfs, se);
    }
    if (cfs->curr == se) { cfs->curr = 0; }
}

void cfs_rq_init(struct cfs_rq *cfs) {
    cfs->tasks_timeline = RB_ROOT_CACHED;
    cfs->nr_running   = 0;
    cfs->min_vruntime = 0;
    cfs->avg_vruntime = 0;
    cfs->avg_load     = 0;
    cfs->curr         = 0;
    cfs->fifo_seq     = 0;
}

// Make `t` runnable on `rq` (wake / new / migrated-in).
void fair_enqueue(struct rq *rq, struct thread *t) {
    enqueue_entity(&rq->cfs, &t->se);
}

// Pick the entity to run next. Returns the thread, and installs it as
// rq->cfs.curr (removing it from the tree). Returns 0 if the rq has no
// runnable fair task -- the caller then tries work-stealing / idle.
// Must be called with rq->lock held and rq->clock_task current.
struct thread *fair_pick(struct rq *rq) {
    struct cfs_rq *cfs = &rq->cfs;

    update_curr(cfs);

    if (cfs->nr_running == 0) {
        if (cfs->curr) { cfs->curr = 0; }
        return 0;
    }

    struct sched_entity *se = pick_eevdf(cfs);
    if (!se) { return 0; }

    if (se != cfs->curr) {
        set_next_entity(cfs, se);
    }
    se->exec_start = rq->clock_task;
    return task_of(se);
}

// The blocking/exiting current thread leaves the fair class. Called
// from schedule() before fair_pick when prev is no longer RUNNING.
void fair_block_current(struct rq *rq, struct thread *prev) {
    dequeue_entity(&rq->cfs, &prev->se);
}

// The preempted current thread goes back in the tree. Called from
// sched_post_switch() once its context is saved (on_cpu clear).
void fair_requeue_preempted(struct rq *rq, struct thread *prev) {
    put_prev_entity(&rq->cfs, &prev->se);
}

// Take the earliest-deadline in-tree entity off `rq` for work-stealing.
// Called with rq->lock held (steal_work holds both rq locks).
struct thread *fair_steal(struct rq *rq) {
    struct cfs_rq *cfs = &rq->cfs;
    struct sched_entity *se = pick_first_entity(cfs);
    if (!se) { return 0; }
    rq_clock_update(rq);       // the victim's clock may be stale
    dequeue_entity(cfs, se);   // full dequeue: it leaves this rq entirely
    return task_of(se);
}

// A stolen thread joins this rq. schedule() then re-picks (it will be
// selected -- it's the only or most-eligible runnable entity).
void fair_accept_stolen(struct rq *rq, struct thread *t) {
    enqueue_entity(&rq->cfs, &t->se);
}

// ---- tick / preemption (SCH-1 Task 4) -------------------------------
//
// EEVDF preemption is event-driven: fair_pick tells the timer code how
// long the picked task may run (fair_slice_remaining_ns), which arms a
// one-shot LAPIC timer. When it fires, timer_handler calls sched_tick
// -> fair_entity_tick, which decides whether to actually switch.

// A task that just got the CPU is not preempted for a marginally
// earlier-deadline waiter until it has run this long -- stops two
// near-equal tasks from trading the CPU every interrupt.
#define SCHED_MIN_PREEMPT_NS 100000ULL   // 0.1 ms

#define SCHED_HOUSEKEEPING_NS 10000000ULL  // 10 ms fallback tick

int fair_entity_tick(struct rq *rq) {
    struct cfs_rq *cfs = &rq->cfs;
    update_curr(cfs);

    struct sched_entity *curr = cfs->curr;
    if (!curr || cfs->nr_running <= 1) { return 0; }

    uint64_t ran = curr->sum_exec_runtime - curr->prev_sum_exec_runtime;
    if (ran >= slice_of(curr)) { return 1; }          // slice spent
    if (ran < SCHED_MIN_PREEMPT_NS) { return 0; }     // anti-thrash floor

    struct sched_entity *first = pick_eevdf(cfs);
    if (first && first != curr && vtime_before(first->deadline, curr->deadline)) {
        return 1;                                     // someone more urgent
    }
    return 0;
}

uint64_t fair_slice_remaining_ns(struct rq *rq) {
    struct cfs_rq *cfs = &rq->cfs;
    struct sched_entity *curr = cfs->curr;
    if (!curr || cfs->nr_running <= 1) { return SCHED_HOUSEKEEPING_NS; }

    uint64_t ran = curr->sum_exec_runtime - curr->prev_sum_exec_runtime;
    uint64_t sl  = slice_of(curr);
    if (ran >= sl) { return SCHED_MIN_PREEMPT_NS; }
    return sl - ran;
}

// ---- selftest hooks (used by kernel/sched/eevdf_selftest.c) ----------

uint64_t eevdf_test_V(struct cfs_rq *cfs) { return avg_vruntime(cfs); }
int64_t  eevdf_test_lag(struct cfs_rq *cfs, struct sched_entity *se) {
    return (int64_t)(avg_vruntime(cfs) - se->vruntime);
}
void eevdf_test_place(struct cfs_rq *cfs, struct sched_entity *se, int initial) {
    place_entity(cfs, se, initial);
}
void eevdf_test_enqueue(struct cfs_rq *cfs, struct sched_entity *se) {
    enqueue_entity(cfs, se);
}
void eevdf_test_dequeue(struct cfs_rq *cfs, struct sched_entity *se) {
    dequeue_entity(cfs, se);
}
void eevdf_test_set_curr(struct cfs_rq *cfs, struct sched_entity *se) {
    set_next_entity(cfs, se);
    se->exec_start = rq_of(cfs)->clock_task;
}
void eevdf_test_advance(struct cfs_rq *cfs, uint64_t now_ns) {
    // simulate the rq clock advancing and charge curr
    rq_of(cfs)->clock = now_ns;
    rq_of(cfs)->clock_task = now_ns;
    update_curr(cfs);
}
struct sched_entity *eevdf_test_pick(struct cfs_rq *cfs) { return pick_eevdf(cfs); }
uint64_t eevdf_test_min_vruntime(struct cfs_rq *cfs) { return cfs->min_vruntime; }
