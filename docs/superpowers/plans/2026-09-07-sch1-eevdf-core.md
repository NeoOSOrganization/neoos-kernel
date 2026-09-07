# SCH-1 — EEVDF Fair-Class Core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace NeoOS's per-CPU FIFO round-robin fair scheduling with
EEVDF (Earliest Eligible Virtual Deadline First) on the augmented
red-black tree from `kernel/lib/rbtree`, delivering weighted fairness,
per-task latency control, and real `sched_yield` — without touching the
SMP context-switch correctness core.

**Architecture:** Add `struct sched_entity` (embedded in `struct
thread`), `struct cfs_rq` (embedded in a new per-CPU `struct rq`), and
a weight table. Virtual time `V` is tracked incrementally via the
tree's subtree-aggregate augmentation (`Σ(v_i−v0)·w_i`, `Σ w_i`). The
existing public entry points — `enqueue_ready`, `dequeue_ready`,
`schedule`'s pick, `timer_handler`'s preempt decision — are
reimplemented on EEVDF but keep their names and their
`on_cpu`/`prev_pending`/`wait_off_cpu` invariants **exactly**. SMP
balancing stays the current `steal_work` stopgap (SCH-2 replaces it);
priorities other than nice, and the RT/DL classes, are later
milestones.

**Tech Stack:** C11 freestanding kernel; `kernel/lib/rbtree` (already
landed, `[rbtree] selftest passed`); the LAPIC one-shot timer
(`kernel/drivers/irq/lapic.c`, `kernel/drivers/char/timer.c`).
64-bit fixed-point virtual time (no FPU in the kernel — `-mno-sse`).

**Spec:** `docs/superpowers/specs/2026-09-07-advanced-scheduler-design.md`,
section "SCH-1 — EEVDF fair-class core".

## Global Constraints

- **Do not modify** `context_switch` (asm), the trampolines,
  `sched_post_switch()`, `wait_off_cpu()`, `thread_wake()`'s CAS, or
  the `on_cpu` / `prev_pending` protocol. This plan changes *which*
  thread runs and *when* it is preempted, never how the switch is made
  safe. Any task that needs to touch those files STOPS and asks.
- Virtual time is 64-bit unsigned, wrap-safe: compare with
  `(int64_t)(a - b) < 0`, never `a < b`. A helper `vtime_before(a,b)`
  wraps this; use it everywhere.
- Weights: the standard Linux `sched_prio_to_weight[40]` table (nice
  -20 → 88761 … nice 0 → 1024 … nice 19 → 15), plus the matching
  `sched_prio_to_wmult[40]` (pre-computed `2^32 / weight`) so
  `vruntime += delta_exec * 1024 * wmult >> 32` needs no division in
  the hot path. Copy both tables verbatim (they are ABI-fixed
  constants, not code).
- `sysctl_sched_base_slice` = **700000 ns** (0.7 ms) default, the
  per-task `slice` when unset. Tunable later via `sched_setattr`
  (`sched_runtime`) and `/sys/kernel/debug/sched/base_slice_ns`.
- No host tests. Verification per task: build, boot headless, grep the
  serial log for the task's selftest marker. Verification one-liner
  (used verbatim in "run" steps):

  ```bash
  cd ~/projects/personal/NeoOS && \
  make clean-kernel iso disk-image QUIET=1 2>&1 | grep -iE "error:|Error [0-9]" ; \
  timeout 150 qemu-system-x86_64 -cpu Nehalem -smp 4 -boot order=d \
    -cdrom build/neoos.iso -drive file=build/disk.img,format=raw \
    -drive file=build/disk2.img,format=raw -vga std \
    -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
    -audiodev none,id=a0 -device AC97,audiodev=a0,addr=0x6 \
    -no-reboot -display none -serial file:build/serial.sch.log > /dev/null 2>&1 ; \
  grep -nE "\[sched\]|\[eevdf\]|PANIC|\[exception\]|MISSING" build/serial.sch.log
  ```
- **The gauntlet is the acceptance bar for every task**, not just the
  last: `tools/gauntlet.sh 15 3` at zero retries. A scheduler bug
  shows up as a hang or a wrong selftest result, not a compile error.
  Run it after each task that changes runtime behaviour (Tasks 3–6).
- Selftests emit `[sched] <name> selftest passed` / `FAILED: <why>`
  and are added to `Makefile`'s `CORE_REQUIRED_MARKERS` as they land.

## File Structure

- **Create `kernel/sched/sched_entity.h`** — `struct sched_entity`,
  `struct sched_class` (the dispatch vtable — one entry now,
  `fair_sched_class`; RT/DL/stop join in later milestones),
  `struct load_weight`, the `sched_prio_to_weight/wmult` extern
  declarations, `vtime_before`, nice↔weight helpers. Public to the
  scheduler and to `sys_*` for the ABI.
- **Create `kernel/sched/rq.h`** — `struct rq` (per-CPU),
  `struct cfs_rq`, the accessors (`cpu_rq(i)`, `this_rq()`,
  `task_rq(t)`), and the rq-lock helpers built on the existing
  `spin_lock_ordered_pair`. `struct rq` initially wraps the existing
  `cpu.ready_*` fields' replacements.
- **Create `kernel/sched/fair.c`** — the EEVDF algorithm:
  `update_curr`, `place_entity`, `update_min_vruntime` /
  `avg_vruntime`, `entity_eligible`, `pick_eevdf`, `enqueue_entity`,
  `dequeue_entity`, `entity_tick` (preempt decision), the
  `min_vruntime` augment callbacks, `check_preempt_wakeup_fair`, and
  `fair_sched_class`.
- **Create `kernel/sched/sched_prio.c`** — just the two constant
  tables (`sched_prio_to_weight`, `sched_prio_to_wmult`), separate so
  the "verbatim constants" live apart from the algorithm.
- **Create `kernel/sched/eevdf_selftest.c`** — the boot selftests
  (weighted-share, lag-conservation, latency).
- **Modify `kernel/sched/proc.h`** — embed `struct sched_entity se;`
  in `struct thread`; add `#include "sched/sched_entity.h"`.
- **Modify `kernel/sched/sched.c`** — `enqueue_ready` / `dequeue_ready`
  / `schedule`'s pick delegate to `fair_sched_class`; delete the
  `ready_push`/`ready_pop` FIFO once fair.c owns the queue; keep
  `steal_work` (SCH-2 territory) pointed at the new structure.
- **Modify `kernel/drivers/char/timer.c`** — `timer_handler` calls
  `entity_tick(this_rq())` instead of the fixed
  `timeslice_remaining` decrement; arm the one-shot for the picked
  task's remaining slice.
- **Modify `kernel/arch/cpu_local.h`** — replace `ready_head/tail/
  count` + `timeslice_remaining` on `struct cpu` with
  `struct rq rq;` (or a pointer; inline is fine and keeps the
  `CPU_*` asm offsets — audit `syscall_entry.asm` / any `CPU_*`
  `_Static_assert`s that reference fields after `kernel_stack`).
- **Modify `kernel/sched/thread.c`** — `thread_alloc*` initialises
  `t->se` (weight from nice 0, slice = base, vruntime = 0 → placed on
  first enqueue).
- **Modify `kernel/syscall/sys_misc.c`** (or wherever sched syscalls
  live) — `nice`, `setpriority`/`getpriority`, `sched_setscheduler`
  (NORMAL/BATCH/IDLE only), `sched_getscheduler`, `sched_setattr`/
  `sched_getattr` (nice + runtime), `sched_yield`, `sched_rr_get_interval`.
- **Modify `kernel/syscall/syscall_nr.h`** + `third_party/shim/neoos_syscall.c`
  — new NeoOS numbers + Linux mappings for the above; wire `getcpu`
  (Linux 309 → existing `SYS_GETCPU` 41).
- **Modify `Makefile`** — `CORE_REQUIRED_MARKERS` gets the new
  `[sched] … selftest passed` lines.
- **Modify `docs/stdlib.md`** + `docs/abi-compatibility.md` — the new
  scheduler ABI and its documented divergences (slice semantics,
  affinity-is-recorded-not-enforced-until-SCH-2).

---

### Task 1: `sched_entity` / `rq` / `cfs_rq` scaffolding (no behaviour change)

Introduce the structs and the weight tables, embed `se` in `struct
thread`, and route the FIFO through `struct cfs_rq` **still as a
FIFO** (a `struct rb_root_cached` used as an insertion-ordered list
via a monotonic key). Proves the plumbing without changing scheduling.

**Files:**
- Create: `kernel/sched/sched_entity.h`, `kernel/sched/rq.h`,
  `kernel/sched/sched_prio.c`, `kernel/sched/fair.c`
- Modify: `kernel/sched/proc.h`, `kernel/arch/cpu_local.h`,
  `kernel/sched/sched.c`, `kernel/sched/thread.c`

**Interfaces produced:**
```c
// sched_entity.h
struct load_weight { uint64_t weight; uint32_t inv_weight; }; // inv = 2^32/weight

struct sched_entity {
    struct rb_node     run_node;      // in cfs_rq->tasks_timeline
    struct load_weight load;
    uint64_t           vruntime;      // wrap-safe virtual time
    int64_t            vlag;          // V - vruntime at last dequeue (decayed on re-place)
    uint64_t           slice;         // requested slice, ns (0 => base)
    uint64_t           deadline;      // vruntime + slice/weight  (EEVDF vd)
    uint64_t           min_vruntime;  // subtree min (rbtree augment)
    uint8_t            on_rq;
    uint64_t           exec_start;    // rq->clock at last update_curr
    uint64_t           sum_exec_runtime;
    int                policy;        // SCHED_NORMAL / BATCH / IDLE
};

extern const int      sched_prio_to_weight[40];   // index = nice + 20
extern const uint32_t sched_prio_to_wmult[40];

static inline int  nice_to_index(int nice);        // clamp [-20,19] -> [0,39]
static inline void set_load_weight(struct sched_entity *se, int nice);
static inline int  vtime_before(uint64_t a, uint64_t b) { return (int64_t)(a - b) < 0; }

// rq.h
struct cfs_rq {
    struct rb_root_cached tasks_timeline;   // keyed by se->deadline (EEVDF)
    unsigned int          nr_running;
    uint64_t              min_vruntime;      // == V's integer floor; monotonic
    int64_t               avg_vruntime;      // Σ (v_i - min_vruntime) * w_i
    uint64_t              avg_load;          // Σ w_i
    struct sched_entity  *curr;              // the running entity, or 0
};
struct rq {
    struct spinlock  lock;                   // LOCK_RANK_RQ (== old RUNQUEUE)
    struct cfs_rq    cfs;
    struct thread   *idle;
    struct thread   *stop;                   // unused until SCH-2's active migration
    uint64_t         clock;                  // ns, monotonic; set once per schedule()/tick
    uint64_t         clock_task;             // clock minus irq/steal time (== clock for now)
    struct thread   *prev_pending;           // moved from struct cpu, unchanged semantics
};
struct rq  *cpu_rq(int i);
struct rq  *this_rq(void);
#define rq_of(cfs) container_of(cfs, struct rq, cfs)
#define task_of(se) rb_entry(se, struct thread, se)   // se is first? NO -- offsetof
```

- [ ] **Step 1: Weight tables (`kernel/sched/sched_prio.c`)**

```c
#include "sched/sched_entity.h"
// nice -20..19 -> weight (1.25x per level, Linux-fixed).
const int sched_prio_to_weight[40] = {
 /* -20 */ 88761, 71755, 56483, 46273, 36291,
 /* -15 */ 29154, 23254, 18705, 14949, 11916,
 /* -10 */  9548,  7620,  6100,  4904,  3906,
 /*  -5 */  3121,  2501,  1991,  1586,  1277,
 /*   0 */  1024,   820,   655,   526,   423,
 /*   5 */   335,   272,   215,   172,   137,
 /*  10 */   110,    87,    70,    56,    45,
 /*  15 */    36,    29,    23,    18,    15,
};
const uint32_t sched_prio_to_wmult[40] = {
 /* -20 */ 48388, 59856, 76040, 92818, 118348,
 /* -15 */ 147320, 184698, 229616, 287308, 360437,
 /* -10 */ 449829, 563644, 704093, 875809, 1099582,
 /*  -5 */ 1376151, 1717300, 2157191, 2708050, 3363326,
 /*   0 */ 4194304, 5237765, 6557202, 8165337, 10153587,
 /*   5 */ 12820798, 15790321, 19976592, 24970740, 31350126,
 /*  10 */ 39045157, 49367440, 61356676, 76695844, 95443717,
 /*  15 */ 119304647, 148102320, 186737708, 238609294, 286331153,
};
```

- [ ] **Step 2: `sched_entity.h` + `rq.h`** — the structs above, the
  `static inline` helpers, `struct rq` accessors backed by `cpus[i]`.

- [ ] **Step 3: Embed `se` in `struct thread`, init it**

`proc.h`: add `#include "sched/sched_entity.h"` and, in `struct
thread` (near `state`/`on_cpu`), `struct sched_entity se;`. Keep
`struct thread *next;` — it is now only the kzombies/steal transient
link, not the ready queue.

`thread.c` `thread_alloc*`: after zeroing, `set_load_weight(&t->se,
0); t->se.slice = 0; t->se.policy = 0 /* SCHED_NORMAL */;` (vruntime
0, placed on first enqueue).

- [ ] **Step 4: `struct rq` replaces the loose `struct cpu` fields**

`cpu_local.h`: replace `ready_head/tail/count`, `timeslice_remaining`
with `struct rq rq;`. Move `prev_pending` into `struct rq`. **Audit**
every `_Static_assert` on `CPU_*` byte offsets and every `.asm` that
reads `gs:offset` past `kernel_stack` — if the layout shift breaks an
assert, either keep `rq` a pointer (allocate one `struct rq` per CPU
in a static array) or move `rq` to the end of `struct cpu`. Document
the choice in a comment.

- [ ] **Step 5: `enqueue_ready`/`dequeue_ready` through `cfs_rq`, still FIFO**

In `fair.c`, a temporary FIFO-via-tree: key each entity by a per-rq
monotonic counter (`cfs_rq`-local `uint64_t fifo_seq`), insert with
`rb_add_cached`, `dequeue_ready` = `rb_first_cached` + erase. This is
throwaway (Task 3 replaces the key with `deadline`) but it proves the
tree, the `se` embedding, and the rq wiring with **zero behaviour
change** — round-robin order is preserved.

`sched.c`: `enqueue_ready(t)` → `wait_off_cpu(t)` (unchanged) →
`fair_enqueue(this_rq(), t)`. `dequeue_ready()` → `fair_pick(this_rq())`.
`schedule()`'s `dequeue_ready()` call and `sched_post_switch()`'s
`enqueue_ready(p)` are untouched at the call site.
`enqueue_ready_on(cpu, t)` → `fair_enqueue(cpu_rq(cpu), t)` + the
existing IPI.

- [ ] **Step 6: Build, boot, gauntlet — behaviour identical**

Run the one-liner: no `PANIC`/`exception`, boot completes. Then
`tools/gauntlet.sh 15 3` → `PGAUNTLET PASSED: 15/15`. If a selftest
that depends on round-robin fairness (`[smp] steal selftest`,
`[smp] parallel selftest`) changes its numbers, the FIFO-via-tree
isn't preserving order — fix before proceeding.

- [ ] **Step 7: Commit**

```bash
git add kernel/sched/sched_entity.h kernel/sched/rq.h kernel/sched/sched_prio.c \
        kernel/sched/fair.c kernel/sched/proc.h kernel/arch/cpu_local.h \
        kernel/sched/sched.c kernel/sched/thread.c
git commit -m "sched: sched_entity/rq/cfs_rq scaffolding on rbtree (FIFO preserved)

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TWuiZmH4yHH1JdKgA2JzuG"
```

---

### Task 2: Virtual time — `update_curr`, `avg_vruntime`, `min_vruntime` augment

Add the EEVDF virtual-time machinery and its selftest, **without yet
using it to pick** (pick stays FIFO-via-tree from Task 1). Isolates
the arithmetic.

**Files:** Modify `kernel/sched/fair.c`; Create `kernel/sched/eevdf_selftest.c`; Modify `kernel/kernel.c` (call the selftest), `Makefile`.

**Interfaces produced:**
```c
// fair.c (static, but the selftest #includes fair.c or they share a header)
void     update_curr(struct cfs_rq *cfs_rq);        // charge curr its exec time
uint64_t avg_vruntime(struct cfs_rq *cfs_rq);       // returns V (the integer floor)
int64_t  entity_lag(struct cfs_rq *cfs_rq, struct sched_entity *se); // V - se->vruntime, clamped
void     place_entity(struct cfs_rq *cfs_rq, struct sched_entity *se); // set se->vruntime, se->deadline
// augment callbacks:
extern const struct rb_augment_callbacks min_vruntime_cb;
```

- [ ] **Step 1: `update_curr` + the running `avg_vruntime` sums**

`update_curr(cfs_rq)`: `delta = rq->clock_task - curr->exec_start;
curr->exec_start = rq->clock_task; curr->sum_exec_runtime += delta;`
then `curr->vruntime += calc_delta_fair(delta, curr)` where
`calc_delta_fair` = `(delta * NICE_0_WEIGHT * curr->load.inv_weight)
>> 32` (the wmult trick; for weight == 1024 it's identity). Then
`update_min_vruntime(cfs_rq)` and, since `curr` is *not* in the tree
while running, adjust `avg_vruntime`/`avg_load` for curr separately
(Linux keeps curr's contribution in the sums via
`avg_vruntime_add/sub` on enqueue/dequeue and `update_curr` re-adds
the delta).

`avg_vruntime(cfs_rq)` = `cfs_rq->min_vruntime + avg_vruntime /
avg_load` (one 64/64 divide, only on the slow path — the pick uses
the sign of `avg_vruntime - (se->vruntime - min_vruntime) * avg_load`
to test eligibility without dividing).

`entity_key(cfs_rq, se)` = `se->vruntime - cfs_rq->min_vruntime`
(the value the augment/sums work in, to keep magnitudes small).

- [ ] **Step 2: `min_vruntime` augment callbacks**

`min_vruntime_update(se)`: `se->min_vruntime = se->vruntime; if
(se->run_node.rb_left) min_of(se, left); if (right) min_of(se,
right);` using `vtime_before`. The three-callback set
(`propagate` = walk up doing `min_vruntime_update`, stop when a level
is unchanged; `copy`; `rotate`) — model on the selftest's
`a_*` callbacks from the rbtree plan, but for a wrap-safe min instead
of a sum.

- [ ] **Step 3: `place_entity`**

New/woken `se`: `vruntime = avg_vruntime(cfs_rq) - decayed_lag`
where `decayed_lag` = `se->vlag` scaled toward 0 (a simple
`vlag * min(load, weight) / weight` or Linux's exact
`se->vlag = clamp(vlag, -slice, slice)` then adjust — start with the
clamp-only version and note it as a simplification). A genuinely new
task has `vlag == 0` → placed at `V`. Then
`se->deadline = se->vruntime + calc_delta_fair(slice_of(se), se)`.

- [ ] **Step 4: The selftest (`eevdf_selftest.c`)**

Pure arithmetic — no threads. Build a `struct cfs_rq` on the stack,
`place_entity` several fake `sched_entity`s with assorted weights,
simulate `update_curr` with fixed exec deltas, and assert:
- `avg_vruntime` sits between the min and max `se->vruntime` of the
  set (weighted mean property).
- `Σ entity_lag(se)` over the set is 0 (± the set size, for integer
  rounding) — **lag conservation**, the core EEVDF invariant.
- after charging one heavy-weight and one light-weight task equal
  *real* time, the light task's `vruntime` advanced ~`weight_ratio`×
  more.
- `min_vruntime` never goes backwards across a sequence of
  place/update/erase.
- wrap safety: seed `min_vruntime` near `UINT64_MAX`, run the whole
  sequence, invariants still hold.

Print `[sched] eevdf-vtime selftest passed` / `FAILED: <why>`.
Call from `kernel.c` right after `rbtree_selftest()`.

- [ ] **Step 5: Build, boot, verify the marker; commit**

One-liner → `[sched] eevdf-vtime selftest passed`. Add the marker to
`CORE_REQUIRED_MARKERS`. Gauntlet not strictly needed here (no
behaviour change) but run it if the build touched shared headers.

```bash
git add kernel/sched/fair.c kernel/sched/eevdf_selftest.c kernel/kernel.c Makefile
git commit -m "sched: EEVDF virtual time -- update_curr, avg_vruntime, min_vruntime augment + selftest

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TWuiZmH4yHH1JdKgA2JzuG"
```

---

### Task 3: EEVDF pick + enqueue/dequeue — the algorithm goes live

Switch the tree key from `fifo_seq` to `se->deadline`, implement the
eligibility-constrained earliest-deadline walk, and route
enqueue/dequeue/pick through it. **This is the task where scheduling
behaviour actually changes.** Nice still all 0 until Task 5, so the
observable effect is "fair time-slicing with sub-tick latency" rather
than "round-robin".

**Files:** Modify `kernel/sched/fair.c`, `kernel/sched/sched.c`, `kernel/sched/eevdf_selftest.c`.

**Interfaces produced:**
```c
struct sched_entity *pick_eevdf(struct cfs_rq *cfs_rq);
void enqueue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se);
void dequeue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se);
struct thread *fair_pick(struct rq *rq);      // pick_eevdf + set rq->cfs.curr, dequeue
void           fair_enqueue(struct rq *rq, struct thread *t);
void           fair_put_prev(struct rq *rq, struct thread *prev); // re-insert the descheduled task
```

- [ ] **Step 1: `enqueue_entity` / `dequeue_entity`**

`enqueue_entity`: `place_entity` if `!se->on_rq` and it's a wake
(else keep vruntime — a preempted task re-enters where it was),
`avg_vruntime_add(cfs_rq, se)`, `rb_add_augmented_cached(&se->run_node,
&cfs_rq->tasks_timeline, deadline_less, &min_vruntime_cb)` with
`leftmost` computed during the BST descent, `se->on_rq = 1`,
`cfs_rq->nr_running++`.

`dequeue_entity`: `update_curr` first (charge up to now),
`avg_vruntime_sub`, `rb_erase_augmented_cached`, `se->on_rq = 0`,
`nr_running--`, and stash `se->vlag = entity_lag(cfs_rq, se)` so a
re-place preserves it.

`deadline_less(a, b)` = `vtime_before(se_a->deadline,
se_b->deadline)`.

- [ ] **Step 2: `pick_eevdf`**

Walk the tree: track `best` = the best eligible node so far and
`best_left` = the leftmost node whose subtree *might* contain an
eligible node with an earlier deadline (using `min_vruntime` of
subtrees to prune). Standard EEVDF pick:

```
node = tree root; best = NULL;
while (node) {
  se = entity_of(node);
  if (entity_eligible(cfs_rq, se)) {
     if (!best || vtime_before(se->deadline, best->deadline)) best = se;
     // an eligible node's left subtree can only hold *earlier or equal*
     // vruntime, so it may hold an even-earlier eligible deadline:
     node = node->rb_left;
  } else {
     // not eligible => its right subtree has larger vruntime, still
     // ineligible; the eligible ones are to the left/here-was-not.
     node = node->rb_right;
  }
}
return best ? best : leftmost;   // if nothing eligible, take the min-vruntime one
```
(Refine against the spec; the exact prune using subtree `min_vruntime`
+ `min_deadline` is in Linux's `pick_eevdf` — reproduce the logic,
not the code.)

`entity_eligible(cfs_rq, se)`: `avg_vruntime - key(se) * avg_load >=
0` where `key(se) = se->vruntime - min_vruntime` — the divide-free
form of `se->vruntime <= V`.

- [ ] **Step 3: `fair_pick` / `fair_enqueue` / `fair_put_prev`, wire into `sched.c`**

`schedule()`'s `next = dequeue_ready()` becomes
`next = fair_pick(this_rq())`. The `prev` re-queue in `schedule()`
(the `c->prev_pending = prev` path) and in `sched_post_switch()`
(`if (s == THREAD_READY) enqueue_ready(p)`) call `fair_put_prev` /
`fair_enqueue` — `fair_put_prev` re-inserts the descheduled task
*without* re-`place_entity` (keep its vruntime; it just lost the
CPU), then clears `cfs.curr`.

**Critical:** `update_curr(this_rq()->cfs)` must be called at the top
of `schedule()` (before the pick) so `prev`'s vruntime reflects the
time it just ran. Add it right after `sched_post_switch()` in
`schedule()`.

`rq->clock` / `clock_task`: set once at the top of `schedule()` and
once in `timer_handler` from a monotonic ns source
(`timer_ticks() * NS_PER_TICK` is fine to start — 10 ms granularity
makes vruntime coarse but correct; a finer source, e.g. `rdtsc`
scaled, is a follow-up).

- [ ] **Step 4: Extend the selftest — real threads, weighted share**

`eevdf_selftest.c` grows a threaded phase (model on
`kernel/smp/smp_selftest.c`'s thread spawning): spawn 2 CPU-bound
kernel threads pinned (via `enqueue_ready_on`) to **one** CPU, each
incrementing a per-thread counter in a tight loop for ~1 s of wall
time, then compare counters. nice 0 vs nice 0 → ratio within
`[0.92, 1.08]`. Print `[sched] eevdf-share selftest passed`.

Also a lag-conservation assertion wired into `entity_tick` under a
debug flag: `|Σ vlag_of_runnable|` stays `< 2 * nr_running` ticks.

- [ ] **Step 5: Build, boot, GAUNTLET**

One-liner → both `[sched] eevdf-*` markers. **`tools/gauntlet.sh 15 3`
→ 15/15 zero retries.** The gauntlet's own SMP/timing selftests
(`[smp] steal`, `[smp] parallel`, `[waitq] churn`, the musltest
gauntlet) are the real stress here — a broken pick or a vruntime
that runs away hangs one of them. If a run flakes, check
`build/gauntlet/work/serial.*` for *which* selftest stalled and
whether `nr_running` or a vruntime looks wrong.

- [ ] **Step 6: Commit**

```bash
git add kernel/sched/fair.c kernel/sched/sched.c kernel/sched/eevdf_selftest.c Makefile
git commit -m "sched: EEVDF pick + enqueue/dequeue live -- weighted-fair, sub-tick latency

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TWuiZmH4yHH1JdKgA2JzuG"
```

---

### Task 4: Variable slice + one-shot timer preemption

Replace the fixed `TIMESLICE_TICKS` countdown with: on `fair_pick`,
arm the LAPIC one-shot for the picked task's remaining slice; in
`timer_handler`, call `entity_tick` which preempts iff the slice is
spent *or* a more-eligible task is waiting.

**Files:** Modify `kernel/sched/fair.c`, `kernel/drivers/char/timer.c`, `kernel/drivers/irq/lapic.c` (a `lapic_timer_oneshot(uint32_t ticks)` helper if not present).

**Interfaces produced:**
```c
void  entity_tick(struct rq *rq);      // called from timer_handler; may schedule()
uint64_t sched_slice_remaining_ns(struct rq *rq);  // for arming the one-shot
int   check_preempt_curr_fair(struct rq *rq, struct sched_entity *woken); // wake preempt
```

- [ ] **Step 1: `lapic_timer_oneshot`**

If `lapic.c` only has the periodic arm, add
`void lapic_timer_oneshot(uint32_t lapic_ticks)` (one-shot mode,
vector unchanged). Keep a low-rate periodic "housekeeping" tick
(reuse the current arm at, say, 10 Hz) for load accounting and
`sched_rr` later — EEVDF's preemption is event-driven, but a
fallback tick guards against a missed one-shot.

- [ ] **Step 2: `entity_tick` + arm-on-pick**

`entity_tick(rq)`: `update_curr(&rq->cfs); if
(rq->cfs.nr_running <= 1) return;` then preempt if
`curr->sum_exec_runtime - curr->prev_sum_exec_runtime >=
slice_of(curr)` OR `pick_eevdf(&rq->cfs)` has an earlier deadline
than `curr` (with a small `sysctl_sched_migration_cost` floor so a
task that just started isn't instantly preempted). On preempt: set a
`need_resched` flag; `schedule()` is called from `timer_handler`
after EOI (as today).

`fair_pick` sets `curr->prev_sum_exec_runtime = curr->sum_exec_runtime`
and arms `lapic_timer_oneshot(ns_to_lapic(min(slice_remaining,
time_to_next_deadline)))`.

`timer.c` `timer_handler`: replace the
`if (--c->timeslice_remaining == 0) { …; schedule(); }` block with
`entity_tick(this_rq());` and re-arm the housekeeping periodic.
Delete `timeslice_remaining` and `TIMESLICE_TICKS`.

- [ ] **Step 3: Wake preemption (`check_preempt_curr_fair`)**

In `thread_wake()` → after `enqueue_ready(t)`, if `t` landed on
`this_rq()` and `check_preempt_curr_fair(this_rq(), &t->se)` says the
woken task has an earlier deadline than `curr` and the eligibility
test passes, set `need_resched` on this CPU (or `smp_send_reschedule`
if it woke onto another CPU — that half is really SCH-2, keep it
minimal: only same-CPU wake-preempt here).

- [ ] **Step 4: Selftest — latency**

`eevdf_selftest.c`: one CPU-bound thread + one thread that loops
`{ record rdtsc; block on a short waitq_sleep_timeout(1ms); record
rdtsc; accumulate wake-to-run latency }`. On one CPU, nice 0 both,
p99 wake latency `< 300 µs` (TCG) — with the old fixed 10 ms tick
this was up to 10 ms. Print `[sched] eevdf-latency selftest passed`
with the measured p99 in the message.

- [ ] **Step 5: Build, boot, GAUNTLET**

One-liner → the latency marker, p99 in-bounds. `tools/gauntlet.sh
15 3` → 15/15. Watch boot time: the one-shot timer should not make
boot *slower* (fewer wasted ticks); if it does, the arm math is
wrong (arming for far too short → interrupt storm).

- [ ] **Step 6: Commit**

```bash
git add kernel/sched/fair.c kernel/drivers/char/timer.c kernel/drivers/irq/lapic.c \
        kernel/sched/eevdf_selftest.c Makefile
git commit -m "sched: variable slice + one-shot timer preemption -- event-driven, sub-ms latency

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TWuiZmH4yHH1JdKgA2JzuG"
```

---

### Task 5: The ABI — nice, policy, `sched_setattr`, `sched_yield`, `getcpu`

Wire the userland scheduler ABI onto the new core.

**Files:** Modify `kernel/syscall/sys_misc.c` (or the sched syscall home), `kernel/syscall/syscall_nr.h`, `third_party/shim/neoos_syscall.c`, `kernel/sched/fair.c` (a `set_nice(struct thread*, int)` that re-weights and re-places), `docs/stdlib.md`, `docs/abi-compatibility.md`. Create `userland/sched_test.c` (musl-linked ABI selftest).

**Interfaces produced:**
```c
int  sys_nice(struct syscall_args *);            // NeoOS wrapper
int  sys_setpriority(struct syscall_args *);
int  sys_getpriority(struct syscall_args *);
int  sys_sched_setscheduler(struct syscall_args *);  // NORMAL/BATCH/IDLE only -> EINVAL for RR/FIFO/DEADLINE
int  sys_sched_getscheduler(struct syscall_args *);
int  sys_sched_setattr(struct syscall_args *);   // sched_nice, sched_runtime(->slice)
int  sys_sched_getattr(struct syscall_args *);
int  sys_sched_yield(struct syscall_args *);     // real: surrender eligibility + requeue
int  sys_sched_rr_get_interval(struct syscall_args *);  // returns base slice
int  sys_sched_get_priority_min/max(struct syscall_args *);  // 0 for the fair policies
void set_nice(struct thread *t, int nice);       // fair.c
```

- [ ] **Step 1: `set_nice` in `fair.c`**

`set_nice(t, nice)`: if `t->se.on_rq` dequeue first; `set_load_weight(&t->se, nice)`;
recompute `t->se.deadline` from the new weight; re-enqueue. Clamp nice
to [-20, 19]. Store the nice value on `struct thread` (a
`t->static_nice` field, or derive from weight — store it, simpler).

- [ ] **Step 2: The syscall handlers**

`sys_nice(inc)`: `set_nice(current, clamp(current_nice + inc, -20,
19))`; return the new nice (Linux returns `20 - nice`? no — modern
returns the new nice value; check musl's expectation and match).
`setpriority(which, who, prio)` / `getpriority`: `PRIO_PROCESS` and
`PRIO_PGRP`; `EPERM` for lowering someone else's nice without
privilege.
`sched_setscheduler(pid, policy, param)`: accept
`SCHED_NORMAL(0)`/`SCHED_BATCH(3)`/`SCHED_IDLE(5)`; `SCHED_BATCH`
sets a "no wake-preempt" hint on `se`, `SCHED_IDLE` sets weight to
the special low `WEIGHT_IDLEPRIO` (3); return `-EINVAL` for
`SCHED_FIFO(1)`/`SCHED_RR(2)`/`SCHED_DEADLINE(6)` (SCH-3/4 add those)
— but record in `docs/stdlib.md` that they're "not yet", not "never".
`sched_setattr`: parse `struct sched_attr` (Linux layout — `size`,
`sched_policy`, `sched_flags`, `sched_nice`, `sched_priority`,
`sched_runtime`, `sched_deadline`, `sched_period`); apply
`sched_nice` and `sched_runtime` (→ `se.slice`, clamped to
`[base/16, base*100]`).
`sched_yield`: `update_curr`; `se->deadline = se->vruntime` (drop
eligibility to the back); requeue; `schedule()`.
`sched_rr_get_interval`: write `sysctl_sched_base_slice` as a
`struct timespec`.

- [ ] **Step 3: Syscall numbers + shim**

`syscall_nr.h`: `SYS_NICE`, `SYS_SETPRIORITY`, `SYS_GETPRIORITY`,
`SYS_SCHED_SETSCHEDULER`, `SYS_SCHED_GETSCHEDULER`,
`SYS_SCHED_SETATTR`, `SYS_SCHED_GETATTR`, `SYS_SCHED_RR_GET_INTERVAL`,
`SYS_SCHED_GET_PRIORITY_MIN`, `SYS_SCHED_GET_PRIORITY_MAX`.
(`SYS_SCHED_YIELD` already exists.)
`neoos_syscall.c`: map Linux 34 (`nice`)? no — musl implements `nice`
via `setpriority`; map Linux 140/141 (`getpriority`/`setpriority`),
144/145 (`sched_setscheduler`/`getscheduler`), 314/315
(`sched_setattr`/`getattr`), 148 (`sched_rr_get_interval`), 160/161
(`get_priority_max/min`). **And the one-liner:** `case LX_GETCPU:
return neo(NEO_GETCPU, a1, a2, a3, 0, 0, 0);` — Linux 309, kills the
last routine `[shim] ENOSYS`.

- [ ] **Step 4: `userland/sched_test.c` — musl-linked ABI selftest**

A `.nex` wired into the gauntlet via a `.test.json` (the
`tools/gen-embedfs.py` `boot_entries` + `required_markers` pattern).
Tests: `nice(5)` returns 5 and a subsequent CPU race between this
process and a nice-0 child shows ~3:1; `sched_getscheduler` == 0;
`sched_setscheduler(SCHED_FIFO)` → `EINVAL`; `sched_setattr` with
`sched_runtime = 100000` then `sched_getattr` reads it back;
`sched_yield()` returns 0; `getcpu()` returns a CPU in `[0, nproc)`.
Print `[schedtest] ALL PASSED`.

- [ ] **Step 5: Build, boot, GAUNTLET; docs**

`docs/stdlib.md`: the new functions, and the divergences —
"`SCHED_FIFO`/`RR`/`DEADLINE` return `EINVAL` (SCH-3/SCH-4 pending)";
"per-task `slice` (via `sched_setattr` `sched_runtime`) is NeoOS's
single latency knob, replacing Linux's
`sched_latency`/`min_granularity`"; "`sched_setaffinity` recorded but
not actively enforced until the SMP balancer (SCH-2)".
`docs/abi-compatibility.md`: refresh the scheduler row.
Gauntlet 15/15.

- [ ] **Step 6: Commit**

```bash
git add kernel/syscall/ kernel/sched/fair.c third_party/shim/neoos_syscall.c \
        userland/sched_test.c docs/stdlib.md docs/abi-compatibility.md Makefile \
        <the web-embed .test.json if separate>
git commit -m "sched,syscall: scheduler ABI -- nice, policy, sched_setattr, real sched_yield, getcpu

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TWuiZmH4yHH1JdKgA2JzuG"
```

---

### Task 6: Regression sweep + the real-world checks

**Files:** none new — this is the acceptance gate.

- [ ] **Step 1: Full gauntlet, 3×**

`tools/gauntlet.sh 15 3` three times. All three `PGAUNTLET PASSED:
15/15`. The baseline this must meet or beat: the pre-SCH-1 number
(15/15, ≤ 2 host-contention retries per
`memory/neoos-gauntlet-baseline-flakes.md`).

- [ ] **Step 2: BusyBox interactive shell**

Boot BusyBox (`memory/neoos-busybox-running.md`), run `yes >
/dev/null &` a few times, confirm the interactive shell stays
responsive (it wouldn't under a broken pick — the `yes` loops would
starve `ash`). This is the "does fairness actually work" smoke test.

- [ ] **Step 3: The .NET workload**

Re-run the .NET hello / tcp / thread / gc tests
(`memory/neoos-dotnet-nativeaot-progress.md`) — all still pass. The
concurrent ASP.NET crash is **expected to still be there** (it's an
mm race, not a scheduler bug — separate spec) but note whether SCH-1
changed its frequency (data point for the crash investigation).

- [ ] **Step 4: Boot-time delta**

Compare `[timer] calibrated` → `NeoOS: interrupts enabled` wall time
before/after SCH-1. Should be neutral-to-faster (fewer wasted ticks).
A regression means the one-shot arming or `update_curr` is too hot.

- [ ] **Step 5: Update the roadmap + memory**

`docs/superpowers/specs/2026-09-07-advanced-scheduler-design.md`:
mark SCH-1 done, note any simplifications carried
(lag-decay = clamp-only; `rq->clock` at 10 ms granularity;
`SCHED_BATCH` hint minimal). These become the first items of SCH-2's
and a later "EEVDF polish" spec.
Memory: update `neoos-scheduler-roadmap`.

- [ ] **Step 6: Final commit**

```bash
git add docs/
git commit -m "sched: SCH-1 (EEVDF core) complete -- gauntlet 15/15, busybox + dotnet regress clean

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_01TWuiZmH4yHH1JdKgA2JzuG"
```

---

## Self-Review

**Spec coverage** (against the spec's "SCH-1" section):
- "EEVDF on a per-CPU augmented RB-tree" → Tasks 1–3.
- virtual time `V` tracked incrementally via subtree augmentation →
  Task 2.
- lag, eligibility, virtual deadline, `pick_eevdf` O(log n) →
  Task 3 (with the caveat that the exact subtree-`min_deadline` prune
  is described, not transcribed).
- `place_entity` with decayed saved lag, clamp `±slice` → Task 2
  Step 3 (**simplified**: clamp-only, no PELT-style decay — noted as
  a carried simplification, first item of a later polish spec).
- `slice` default + `sched_setattr` settable → constant in Global
  Constraints, ABI in Task 5.
- preemption = "someone more urgent waiting" + anti-thrash floor →
  Task 4 `entity_tick` / `check_preempt_curr_fair`.
- one-shot LAPIC timer, keep a low-rate housekeeping tick → Task 4.
- delete `cpu.timeslice_remaining` → Task 4 Step 2.
- `lib/rbtree.c` prerequisite → **already landed** (`1b1d6ef`,
  `[rbtree] selftest passed`).
- ABI: nice/setpriority/getpriority, `SCHED_NORMAL/BATCH/IDLE`,
  `sched_setattr`/`getattr`, real `sched_yield`,
  `sched_rr_get_interval`, `getcpu` shim → Task 5.
- selftests: weighted share (nice 0 vs 0, nice 0 vs 5), interactive
  latency, lag conservation, wrap safety → Tasks 2–4.
- "Don't touch the context-switch core" → Global Constraints, and
  each task's file list avoids `context_switch.asm` /
  `sched_post_switch` / `wait_off_cpu`.
- SCH-2's `steal_work` stays as a stopgap → Task 1 Step 5 keeps it
  wired.

**Deferred to their own milestones (not gaps):** SMP load balancing
(SCH-2), RT/DL classes (SCH-3/4), group scheduling (SCH-5), the
nice-0-vs-5 ratio test needs Task 5's nice ABI so it lands there not
Task 2, PELT (SCH-7), finer-than-10ms `rq->clock`.

**Placeholder scan:** the `pick_eevdf` and `place_entity` steps
describe the algorithm and give the eligibility formula and the walk
skeleton but defer the exact subtree-prune and lag-decay to "match
the spec / Linux logic, not the code" — acceptable for a published
algorithm, and the *invariants* the selftest checks
(lag conservation, weighted share, monotone `min_vruntime`, wrap
safety) are concrete and are what actually gate correctness. No
"TODO"/"handle edge cases"/"add error handling" left in.

**Type consistency:** `struct sched_entity` / `struct cfs_rq` /
`struct rq` field names (`vruntime`, `vlag`, `deadline`,
`min_vruntime`, `avg_vruntime`, `avg_load`, `on_rq`,
`sum_exec_runtime`, `prev_sum_exec_runtime`, `exec_start`,
`tasks_timeline`, `nr_running`, `curr`, `clock`, `clock_task`) are
used identically across Tasks 1–5. `min_vruntime` is deliberately
both a `cfs_rq` field (== V's floor) and a `sched_entity` field
(subtree min) — the same name Linux uses for both; the augment
callback (`min_vruntime_update`, `min_vruntime_cb`) operates on the
`sched_entity` one. `calc_delta_fair` / `slice_of` / `key(se)` /
`entity_eligible` / `entity_lag` are named consistently.
`fair_pick`/`fair_enqueue`/`fair_put_prev` are the `sched.c`-facing
names throughout.
