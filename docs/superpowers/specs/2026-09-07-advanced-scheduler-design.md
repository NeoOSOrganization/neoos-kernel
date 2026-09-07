# Advanced Scheduler — Design Roadmap (EEVDF, SMP, NUMA, full policy set)

## Why

NeoOS's scheduler today (`kernel/sched/sched.c`, ~466 lines) is a
**pure per-CPU FIFO round-robin**:

- Per-CPU ready queues (`cpu.ready_head/tail/count`), each a plain
  singly-linked list, `ready_push`/`ready_pop` at the ends.
- One fixed quantum: `cpu.timeslice_remaining`, decremented by the
  100 Hz LAPIC tick. No per-task slice, no interactivity credit.
- No priority of any kind — every thread is equal. No nice, no
  realtime, no deadline.
- Load balancing is one reactive path: `steal_work()` pulls **one**
  thread from the numerically-busiest peer queue, and only when the
  local queue is empty AND the CPU reaches its own next tick (up to
  10 ms of latency, no push-IPI on enqueue).
- No topology model beyond a flat CPU list. `smp_topology_init()`
  exists but records only count + APIC ids. Several files hardcode
  "exactly one NUMA node, always" (`syscall_nr.h:241`,
  `sys_misc.c:229`).

This is fine for the selftest gauntlet and a single interactive shell.
It is **not** fine for the workloads NeoOS now runs: the ASP.NET Core
milestone put a .NET thread pool (GC threads, IO threads, worker
threads, timer/gate threads) on the machine, and concurrent request
handling is where things fall over. Round-robin with 10 ms steal
latency gives terrible tail latency for a request/response server, and
the absence of priorities means a busy background thread starves the
Kestrel accept loop just as badly as it starves everything else.

**Goal**: a mature, Linux-class scheduler — EEVDF as the fair-class
core, a full policy stack on top (realtime, deadline, group
scheduling), real SMP load balancing over a sched-domain topology, and
NUMA awareness end to end. Fast in the two senses that matter: low
scheduling **overhead** (the hot path stays O(log n) with small
constants and few cache misses) and low scheduling **latency** (a woken
task that should preempt does so within microseconds, not one tick).

Plus one **NeoOS-native** capability with no Linux equivalent (SCH-9):
a syscall that converts the scheduler into a hard-realtime mode — one
process is handed a CPU, or the whole machine, exclusively; every other
task stops getting CPU time; preemption, the tick, and migration are
turned off for the duration; a kernel-clamped deadman timer and a
hardware-watchdog fallback guarantee the machine always comes back.

## Global constraints (NeoOS conventions)

- Milestone decomposition per `CLAUDE.md`: this document is the
  **roadmap**. Each `SCH-n` below gets its own
  `docs/superpowers/specs/` design + `docs/superpowers/plans/`
  implementation plan before code. This spec fixes the shape, the
  data structures that cross milestone boundaries, and the ABI.
- The kernel's internals are ours (`docs/abi-compatibility.md`): the
  runqueue data structures, the class dispatch, lock ranks, the
  balancer's internal calling convention may be designed freely.
- What is **observable from userland must be Linux-shaped**:
  `sched_setscheduler`/`sched_setattr`/`sched_getattr`,
  `sched_setaffinity`/`getaffinity`, `sched_get_priority_min/max`,
  `nice`, `getpriority`/`setpriority`, `sched_yield`,
  `sched_rr_get_interval`, the `SCHED_*` policy numbers and their
  semantics, `struct sched_attr`'s layout, the cgroup-v2 `cpu.*`
  control files, `/proc/[pid]/stat` scheduler fields,
  `/proc/schedstat`, and `getcpu`. Every deliberate deviation goes in
  `docs/stdlib.md`.
- NeoOS's own syscall numbers stay NeoOS's; the musl shim
  (`third_party/shim/neoos_syscall.c`) maps Linux's onto them.
- Verified headless in QEMU per project convention:
  `-smp {1,2,4,8}`, and for NUMA `-numa node,...` +
  `-numa dist,...` topologies. No host-runnable unit tests; the
  scheduler's invariants are checked by in-kernel selftests
  (`kernel/smp/smp_selftest.c` is the model) that run at boot and by
  the gauntlet.
- **Do not regress the gauntlet.** Every `SCH-n` ends with
  `tools/gauntlet.sh 15 3` at zero retries, plus its own new
  selftests.

---

## Architecture overview

### Scheduling classes (dispatch order, highest first)

Linux's `sched_class` chain, kept as-is because applications depend on
the priority ordering between policies:

| class | policies | runqueue | picked when |
|---|---|---|---|
| `stop_sched_class` | (kernel only) | single task | migration/CPU-hotplug stopper is queued |
| `dl_sched_class` | `SCHED_DEADLINE` | per-CPU GEDF tree, keyed by absolute deadline | any dl task is runnable and its budget is not exhausted |
| `rt_sched_class` | `SCHED_FIFO`, `SCHED_RR` | per-CPU 100-level priority array + bitmap | any rt task runnable |
| `fair_sched_class` | `SCHED_NORMAL`, `SCHED_BATCH`, `SCHED_IDLE` | **EEVDF** — per-CPU augmented RB-tree keyed by virtual deadline | nothing higher runnable |
| `idle_sched_class` | (the per-CPU idle thread) | — | nothing else |

`pick_next_task()` walks the chain; in the overwhelmingly common
"only fair tasks" case it is a single indirect call into
`fair_sched_class.pick_next_task` with an "all fair" fast path that
skips the chain walk entirely (Linux's `sched_class_highest` shortcut).

### Per-CPU runqueue (`struct rq`)

Replaces the three loose fields on `struct cpu`. One `struct rq` per
CPU, cache-line aligned, holding:

- `struct cfs_rq cfs` — the EEVDF tree (see SCH-1)
- `struct rt_rq rt` — the rt priority array
- `struct dl_rq dl` — the deadline tree + running bandwidth
- `nr_running`, `nr_uninterruptible`, per-class counts
- `curr`, `idle`, `stop` task pointers
- `clock`, `clock_task`, `clock_pelt` — the rq clocks (see PELT, SCH-7)
- `struct sched_domain *sd` — this CPU's balancing domains (SCH-2)
- `struct rq_flags`-style lock (`rq->lock`, `LOCK_RANK_RUNQUEUE`),
  plus the double-rq-lock helper (`spin_lock_ordered_pair` already
  exists and already documents the address-order rule — reuse it).
- balancing bookkeeping: `next_balance`, `nr_balance_failed`,
  `avg_idle`, `idle_stamp`, `push_cpu`, `active_balance`
- `cpu_capacity`, `cpu_capacity_orig` (SCH-7)
- NUMA: `numa_node` back-pointer (SCH-6)

### `struct sched_entity` (per schedulable unit)

Every thread carries one; group scheduling (SCH-5) adds one per
`task_group` per CPU. Fields:

- `load` (weight derived from nice / cpu.weight)
- `vruntime`, `deleted` — EEVDF virtual time
- `vlag` — lag = `S - v_i` (how far ahead/behind fair share)
- `slice` — the task's requested time slice (base latency; set by
  `sched_setattr`'s `sched_runtime`, default `sysctl_sched_base_slice`
  ≈ 0.7 ms scaled)
- `deadline` — `vruntime + slice/weight` (the EEVDF virtual deadline)
- `min_vruntime` (subtree min, the RB-tree augmentation)
- `on_rq`, `exec_start`, `sum_exec_runtime`, `prev_sum_exec_runtime`
- `struct sched_avg avg` — PELT load/util tracking (SCH-7)
- `struct sched_entity *parent`, `struct cfs_rq *cfs_rq`,
  `*my_q` — group hierarchy links (SCH-5)

### What stays

- `context_switch` (asm), the trampolines, `sched_post_switch()` and
  the `on_cpu` / `prev_pending` / `wait_off_cpu` machinery — this is
  the hard-won SMP-correctness core and the redesign **must not touch
  its invariants**. The new code changes *which* thread
  `schedule()` picks and *when* it preempts, not how the switch
  itself is made safe.
- `THREAD_READY/RUNNING/BLOCKED/ZOMBIE` states.
- The per-CPU LAPIC timer as the preemption tick source (SCH-1 makes
  it a variable-rate / one-shot deadline timer; a later, optional
  step makes it fully tickless).

---

## SCH-1 — EEVDF fair-class core

**Scope**: replace the FIFO fair path with EEVDF on a per-CPU
augmented red-black tree. Single-CPU correct first; SMP balancing is
SCH-2 (until then, keep the existing `steal_work` as a stopgap so
`-smp >1` still uses all cores).

### The algorithm

EEVDF (Earliest Eligible Virtual Deadline First), the model Linux
adopted in 6.6:

- Virtual time `V` advances at `sum(weight_of_runnable)^-1` per unit
  real time — i.e. `V += Δexec / total_weight`. Tracked as
  `cfs_rq->avg_vruntime` / `avg_load` incrementally (the RB-tree
  augmentation carries `Σ(v_i - v0)·w_i` and `Σ w_i` per subtree so
  `V` is O(1) to read and O(log n) to maintain).
- Each task has **lag** `vlag_i = V - v_i`. Positive lag = owed CPU;
  negative = got ahead. Lag sums to zero across the set.
- A task is **eligible** when `v_i <= V` (its lag is ≥ 0).
- On each eligible task compute a **virtual deadline**
  `vd_i = v_i + slice_i / w_i`.
- `pick_next` = the eligible task with the smallest `vd_i`. With the
  augmented tree this is an O(log n) walk: descend, at each node
  choose the left subtree if it contains an eligible task with a
  deadline ≤ the current best, else the node, else the right subtree.
- On enqueue (`place_entity`): a new/woken task is placed with
  `v_i = V - vlag_i` where `vlag_i` is its **decayed** saved lag
  (0 for a genuinely new task) — this is what stops a task from
  gaming the scheduler by sleeping, and what gives a just-woken
  interactive task its fair "catch-up" without letting it monopolise.
  Clamp lag to `±slice` so a task that slept for an hour does not
  come back owed an hour.
- `slice_i` defaults to `sysctl_sched_base_slice` and is settable per
  task via `sched_setattr(sched_runtime=...)`. Smaller slice = more
  frequent, lower-latency scheduling at higher switch cost; larger =
  batch-friendly. This is the single knob that replaces CFS's
  `sched_latency`/`min_granularity`/`wakeup_granularity` tangle.

### Preemption decision

`check_preempt_wakeup` / the tick handler: the current task is
preempted when a runnable task exists whose virtual deadline is
earlier than the current task's, **and** the current task has used at
least `sysctl_sched_migration_cost`-worth of its slice (a small
anti-thrash floor). This replaces "your 10 ms are up" with "someone
more urgent is waiting" — the latency win.

### Quantum / timer

Move from a fixed 100 Hz decrement to: on `pick_next`, arm the LAPIC
timer one-shot for `min(remaining_slice, time_until_next_deadline)`.
The 100 Hz tick stays as a low-rate housekeeping fallback
(load accounting, `sched_rr` rotation) until a later tickless step
removes it. `cpu.timeslice_remaining` is deleted;
`sched_entity.slice` + `deadline` replace it.

### Data-structure work

- Add `lib/rbtree.c` — an augmented intrusive red-black tree
  (`rb_node` embedded in `sched_entity`, augment callbacks for
  `min_vruntime`). NeoOS has no rbtree today; this is the one new
  general-purpose structure and it is reused by `dl_rq` and the
  timer wheel later. ~400 lines, a direct port of the standard
  algorithm, with an in-kernel selftest (insert/delete/iterate 10k
  random keys, verify red-black + augment invariants).
- `struct cfs_rq`: `tasks_timeline` (rb root + leftmost cache),
  `avg_vruntime`, `avg_load`, `nr_running`, `min_vruntime`,
  `curr`.

### ABI delivered by SCH-1

- `nice()` / `setpriority`/`getpriority(PRIO_PROCESS|PRIO_PGRP)`
  → nice ∈ [-20, 19] → weight via the standard `sched_prio_to_weight[]`
  table (1.25× per level).
- `sched_setscheduler`/`sched_getscheduler` for
  `SCHED_NORMAL`/`SCHED_BATCH`/`SCHED_IDLE`.
- `sched_setattr`/`sched_getattr` for `sched_nice`,
  `sched_runtime` (→ slice).
- `sched_yield` = "set `vruntime = V` (surrender eligibility) and
  requeue" — a real yield, not a no-op.
- `sched_rr_get_interval` returns the base slice for a NORMAL task.
- `getcpu(2)` — kernel already has `SYS_GETCPU (41)`; wire the shim
  (Linux 309) to it (see the missing-syscalls spec).

### Selftests

- Two CPU-bound tasks, nice 0 vs nice 0 → 50/50 ± 2% over 2 s.
- nice 0 vs nice 5 → ~3:1 (the weight ratio).
- One CPU-bound + one "sleep 1 ms / run 1 ms" task → the sleeper
  gets ≥ 45% and its wake-to-run latency p99 < 200 µs.
- A task with `slice = 100 µs` vs one with `slice = 10 ms`, both
  nice 0 → equal CPU share, but the small-slice task's scheduling
  latency p99 is an order of magnitude lower.
- Lag conservation: sum of `vlag` across all runnable tasks stays
  within `±1` tick of zero (asserted continuously in the tick).

---

## SCH-2 — SMP load balancing over sched domains

**Scope**: a real balancer. Delete `steal_work`'s "one thread from
the biggest queue" heuristic; replace with Linux's domain-based
periodic + event-driven balancing.

### Sched domains

Built from `smp_topology_init` (extended in SCH-6 to read ACPI SRAT/
SLIT). Levels, innermost first:

- `SMT` (if the CPU exposes sibling threads — QEMU `-smp
  ...,threads=2` does; skip the level if `threads==1`)
- `MC` (last-level-cache / "cluster" — one per socket for the
  machines NeoOS runs on)
- `DIE` / package
- `NUMA` levels (SCH-6 — one per distance in the SLIT)

Each `struct sched_domain` carries `span` (cpumask), `groups`
(balancing granularity), `flags` (`SD_LOAD_BALANCE`,
`SD_BALANCE_WAKE`, `SD_WAKE_AFFINE`, `SD_SHARE_PKG_RESOURCES`,
`SD_NUMA`, …), `min_interval`/`max_interval`, `imbalance_pct`,
`cache_nice_tries`, `busy_factor`.

### The three balancing paths

1. **Periodic** (`rebalance_domains`, from the tick / a
   `SCHED_SOFTIRQ`): each CPU, at its domain's `balance_interval`,
   walks its domains outward, computes per-group load
   (`update_sg_lb_stats` → PELT `runnable_avg` sums, not task
   counts), finds the busiest group, and if the imbalance exceeds
   `imbalance_pct` pulls tasks (`detach_tasks` / `attach_tasks`)
   until balanced or the busiest group's `can_migrate` list is
   exhausted. Respects `sched_migration_cost` (a task that ran
   recently on its current CPU is cache-hot; don't move it unless
   `nr_balance_failed` shows we're stuck).
2. **New-idle** (`newidle_balance`, from `schedule()` when
   `pick_next` finds nothing): a bounded pull attempt from the
   sched-domain hierarchy, gated by `rq->avg_idle` vs
   `sysctl_sched_migration_cost` (don't spend 50 µs balancing if we
   idle for 5 µs at a time). This is the replacement for `steal_work`
   and it is what kills the 10 ms latency: an idle CPU pulls
   **immediately**, in its own `schedule()` call, not on its next
   tick.
3. **Active migration** (`active_load_balance_cpu_stop`): when the
   busiest CPU is running the only task that could be moved,
   `stop_sched_class` is used to kick that task off. This is where
   the `stop` class earns its keep.

### Wake-up placement (`select_task_rq_fair`)

On wake, choose the CPU: prefer an idle sibling in the LLC domain
(`SD_SHARE_PKG_RESOURCES`) near `prev_cpu` (`wake_affine` — was the
waker on a nearby CPU? keep them close for producer/consumer
locality), else `find_idlest_cpu` down the domain tree, else
`prev_cpu`. EAS (SCH-7) overrides this with an energy-aware choice
when an energy model is present.

### Push-on-enqueue

`ttwu_queue` / the enqueue path: when a task is woken onto a CPU that
is **not idle** and there is an idle CPU in a domain that permits
wake-balancing, send that CPU a reschedule IPI (`smp_send_reschedule`
already exists) so it runs `newidle_balance` now. This is the
event-driven half; periodic balancing catches what wakeups miss
(long-running tasks that never sleep).

### Lock rank / correctness

Double-rq lock via the existing `spin_lock_ordered_pair` (address
order = CPU-index order, already documented and proven). `detach`/
`attach` never hold three rq locks. The `on_cpu` /
`wait_off_cpu` invariant from the current code is preserved verbatim —
a migrated task is only enqueued on the destination rq after its
context is saved, exactly as `steal_work` does today.

### Selftests

- 8 CPU-bound tasks on `-smp 4` → each CPU ends with 2 ± 0, load
  spread ≤ 1 task within `2·balance_interval`.
- Producer/consumer pair (futex ping-pong) → `wake_affine` keeps
  them on the same LLC; measured cross-CPU wakeup rate < 5%.
- Idle-pull latency: one CPU busy with 4 tasks, 3 CPUs idle → within
  1 ms all 4 CPUs are running one task each (today: up to 10 ms).
- `sched_migration_cost` respected: a single hot task ping-ponging
  with a 1 ms sleeper does NOT migrate on every wake (migration
  count over 5 s < 10).

---

## SCH-3 — Realtime class (`SCHED_FIFO`, `SCHED_RR`)

**Scope**: a POSIX realtime class that strictly preempts fair.

- `struct rt_rq`: 100 priority levels (rt priority 1..99 → array
  index), a `DECLARE_BITMAP` of non-empty levels, a FIFO list per
  level. `pick` = `find_first_bit` + list head. O(1).
- `SCHED_FIFO`: runs until it blocks, yields, or is preempted by a
  higher rt priority.
- `SCHED_RR`: same, plus a `sched_rr_timeslice` (default 100 ms,
  settable via `/proc/sys/kernel/sched_rr_timeslice_ms`) round-robin
  within a priority level, driven by the housekeeping tick.
- `chrt`-visible: `sched_setscheduler(SCHED_FIFO|SCHED_RR,
  sched_priority ∈ [1,99])`, `sched_get_priority_min/max`.
- **RT throttling**: `sched_rt_runtime_us` / `sched_rt_period_us`
  (default 950 ms / 1000 ms) so a runaway `SCHED_FIFO` loop cannot
  wedge the machine — after the budget is spent the class yields to
  fair for the rest of the period. Per-rq accounting, hierarchical
  once SCH-5 lands.
- SMP: rt has its own balancer (`push_rt_task` / `pull_rt_task` on
  the `cpupri` bitmap — "is there a CPU running something lower
  priority than this newly-runnable rt task?"). rt tasks are pushed
  eagerly on wake/enqueue, not left for periodic balancing —
  latency is the whole point.
- Priority inheritance for `pthread_mutex` with
  `PTHREAD_PRIO_INHERIT`: `rt_mutex` + the PI chain walk. This
  touches `kernel/sync/` and `kernel/ipc/futex.c` (PI futexes:
  `FUTEX_LOCK_PI`/`UNLOCK_PI`/`WAIT_REQUEUE_PI`). Larger sub-piece;
  gets its own sub-spec.

### Selftests

- `SCHED_FIFO` prio 50 task + 4 CPU-bound `SCHED_NORMAL` → the FIFO
  task gets 100% of one CPU on demand, wake latency p99 < 50 µs.
- Two `SCHED_RR` prio 10 → alternate at ~100 ms.
- RT throttle: a `SCHED_FIFO` `while(1)` → machine stays responsive,
  the fair shell still gets ~5% CPU.
- PI: low-prio holder + high-prio waiter + mid-prio spinner →
  holder is boosted, no unbounded priority inversion.

---

## SCH-4 — `SCHED_DEADLINE` (CBS + GEDF)

**Scope**: the EDF/CBS deadline class, strictly above rt.

- `sched_setattr` with `SCHED_DEADLINE` + `sched_runtime`,
  `sched_deadline`, `sched_period`. Constant-Bandwidth-Server: each
  task gets `runtime` every `period`; when the budget hits zero the
  task is throttled until its next replenishment; the absolute
  deadline is pushed by `period` each replenishment.
- `struct dl_rq`: an rb-tree (reuse `lib/rbtree.c`) keyed by
  absolute deadline; `pick` = leftmost. Global-EDF on SMP: a
  newly-runnable dl task preempts the CPU running the latest-deadline
  dl task (or any non-dl task); `push_dl_task`/`pull_dl_task` on a
  `cpudl` max-heap of per-CPU latest deadlines.
- **Admission control**: `sched_setattr(SCHED_DEADLINE)` fails with
  `-EBUSY` if `Σ runtime_i/period_i` over the affinity-relevant CPU
  set would exceed `sched_rt_runtime/sched_rt_period` (default 95%).
  This is mandatory — an over-committed EDF set misses deadlines
  unpredictably.
- Bandwidth reclaiming (GRUB) — optional, SCH-4b: let a dl task use
  slack left by other dl tasks up to a cap, so `runtime` can be set
  conservatively without wasting CPU.
- Interaction: a dl task that forks — child does NOT inherit
  `SCHED_DEADLINE` (Linux semantics: child becomes `SCHED_NORMAL`).

### Selftests

- 3 dl tasks (10 ms / 100 ms each) on 1 CPU → 30% total, all
  deadlines met over 10 s, 4th task rejected by admission control.
- dl + rt + fair → dl always runs first, then rt, then fair.
- dl task that overruns its runtime → throttled exactly at budget,
  no overrun bleed.

---

## SCH-5 — Group scheduling (cgroup v2 `cpu` controller)

**Scope**: hierarchical fair scheduling so a set of processes shares
CPU as a unit — what container runtimes and `systemd` slices need,
and what lets NeoOS cap the .NET thread pool as a group.

- `struct task_group`: a tree rooted at `root_task_group`. Each group
  has, **per CPU**, a `struct cfs_rq` (its own EEVDF tree of that
  group's entities on that CPU) and a `struct sched_entity` (the
  group's representative in its parent's `cfs_rq`).
- `pick_next_task_fair` recurses: pick the top entity in
  `rq->cfs`, if it's a group descend into `se->my_q`, repeat until a
  task. Enqueue/dequeue walk up the chain updating each level's
  load/vruntime.
- cgroup-v2 files, backed by NeoOS's cgroupfs (new — small
  `kernfs`-style pseudo-fs, or a `/sys/fs/cgroup` ramfs overlay):
  - `cpu.weight` (1..10000, default 100 → maps to nice via the
    inverse table), `cpu.weight.nice`
  - `cpu.max` — `"$quota $period"` bandwidth throttling
    (`cfs_bandwidth`: per-group runtime pool refilled by an hrtimer,
    group throttled when exhausted, unthrottled on refill; the
    throttled-cfs-rq list and the `distribute` walk)
  - `cpu.stat` — `usage_usec`, `nr_periods`, `nr_throttled`,
    `throttled_usec`
  - `cpu.pressure` — PSI (SCH-7 dependency; can stub)
- `cgroup` / `clone3(CLONE_INTO_CGROUP)` / the `cgroup.procs` write
  path to move a process between groups.
- RT group scheduling (`cpu.rt_runtime_us` hierarchy) — include if
  SCH-3's PI work didn't already balloon; else defer.

### Selftests

- Group A (weight 100) with 1 task vs group B (weight 100) with 3
  tasks, on 1 CPU → A's task gets 50%, B's three split 50%.
- `cpu.max = "10000 100000"` on a group running `while(1)` → group
  capped at 10% of one CPU, `nr_throttled` increments.
- Nested: A/{A1,A2}, weights → shares multiply down the tree.

---

## SCH-6 — NUMA

**Scope**: topology discovery, NUMA-aware memory, and the automatic
NUMA balancer. Removes the "one node, always" hardcodes.

### Topology

- Parse **ACPI SRAT** (System Resource Affinity Table): CPU→node,
  memory-range→node. Parse **SLIT** (System Locality Information
  Table): the `node_distance[i][j]` matrix. `smp_topology_init`
  extended; new `kernel/mm/numa.c` owns `struct pglist_data`
  (per-node: CPU mask, memory zones, free lists, `kswapd`-equivalent
  later).
- `mm/pmm.c`: the frame allocator becomes per-node. `pmm_alloc()`
  grows a `nid` / `gfp`-style flags argument (default: local node,
  fall back along the SLIT distance order). Page-table pages, kernel
  stacks, and a process's early allocations go to the node of the
  CPU that faulted.
- Sched domains (SCH-2) gain NUMA levels, one per distinct SLIT
  distance, with `SD_NUMA` and progressively larger
  `imbalance_pct` / `cache_nice_tries` so the balancer is
  increasingly reluctant to move a task across node boundaries.

### Automatic NUMA balancing

Linux's scheme, `CONFIG_NUMA_BALANCING`:

- Periodically (`task_numa_work`, from the tick, rate-limited) unmap
  a window of a task's anonymous pages by clearing their PTE
  present bit → PROT_NONE-style. NeoOS's `vma_fault` /
  `paging_handle_cow_fault` path gains a "NUMA hint fault" case.
- On the resulting minor fault: record which node the faulting CPU
  is on and which node the page is on; accumulate per-task and
  per-`numa_group` (tasks that fault on shared pages) statistics
  in `task->numa_faults[]`.
- `task_numa_placement`: pick the task's **preferred node** = the
  node it faults on most. `numa_migrate_preferred` nudges the task
  toward a CPU on that node (via the balancer). `migrate_misplaced_page`
  moves a hot page to the faulting node if the task is settled there.
- `numa_group`: threads of one process that share pages get a group
  preferred node so the .NET thread pool converges on one node
  instead of scattering.
- Knobs: `/proc/sys/kernel/numa_balancing`,
  `numa_balancing_scan_period_{min,max}_ms`,
  `numa_balancing_scan_size_mb`.

### ABI

- `getcpu` returns the real node.
- `get_mempolicy`/`set_mempolicy`/`mbind` become real (today they're
  MPOL_DEFAULT-only stubs — `sys_misc.c:229`). `MPOL_BIND`,
  `MPOL_PREFERRED`, `MPOL_INTERLEAVE`.
- `move_pages`, `migrate_pages`.
- `/sys/devices/system/node/node*/…`, `numactl` works.
- `sched_setaffinity` already needed for Server GC (missing-syscalls
  spec) — here it also gains NUMA-distance-aware default behaviour.

### Testing

QEMU `-numa` topologies:
`-object memory-backend-ram,size=512M,id=m0 -object
memory-backend-ram,size=512M,id=m1 -numa node,memdev=m0,cpus=0-1
-numa node,memdev=m1,cpus=2-3 -numa dist,src=0,dst=1,val=20`.

- Two nodes, a task pinned to node 1 CPUs allocating 100 MB → after
  the scan converges, ≥ 90% of its resident pages are on node 1.
- Unpinned task doing local work → migrates to the node holding its
  working set within ~5 s, cross-node fault rate drops below 10%.
- `numactl --interleave` → pages round-robin across nodes ± 5%.
- Single-node machine (no SRAT, or SRAT with one node) → everything
  behaves exactly as today, zero overhead (the balancer's NUMA
  levels don't exist).

---

## SCH-7 — CPU capacity, PELT, uclamp, EAS

**Scope**: utilization tracking and (where hardware allows) energy-
and capacity-aware placement.

- **PELT** (Per-Entity Load Tracking): geometric decay of
  `load_avg` / `runnable_avg` / `util_avg` over ~1024 µs periods
  per `sched_entity` and per `cfs_rq`. This is the input the SCH-2
  balancer *should* be using from the start; SCH-2 ships with a
  simpler `nr_running`-weighted estimate and SCH-7 upgrades it in
  place. Also feeds `cpu.stat` and PSI.
- **`cpu_capacity`**: `capacity_orig` per CPU from a capacity model.
  On symmetric QEMU everything is 1024; the plumbing exists so an
  asymmetric ("big.LITTLE") `-cpu` mix or a future real board slots
  in. `capacity` (available after subtracting rt/dl/irq pressure)
  gates the balancer: don't pile fair load onto a CPU whose capacity
  is eaten by rt.
- **uclamp**: `sched_setattr`'s `sched_util_min` / `sched_util_max`
  (+ the cgroup `cpu.uclamp.{min,max}` files) — clamp a task's
  effective utilization so a latency-sensitive task requests a
  higher-capacity CPU / OPP even when its measured util is low.
- **EAS** (Energy-Aware Scheduling): only active when an **energy
  model** is registered (per-OPP power tables). `feec()` — for a
  waking task on an asymmetric-capacity machine, pick the CPU that
  fits the task and yields the lowest estimated system energy.
  Requires **cpufreq** (`schedutil` governor), which NeoOS does not
  have — so SCH-7 either brings a minimal ACPI `_PSS` / `MSR`
  P-state driver + `schedutil`, or ships EAS as inert-until-model
  and stops at capacity+uclamp. Decision deferred to SCH-7's own
  spec.

---

## SCH-9 — Hard-realtime "CPU takeover" mode (NeoOS-native)

**Scope**: a NeoOS-specific mode in which one process is handed a CPU
(or the whole machine) *exclusively* — every other task is evicted from
that CPU, the tick and as many interrupts as possible are steered away,
and the process runs uninterrupted with hard, bounded worst-case
latency until it releases the CPU or a deadman timer fires.

This is deliberately **not** Linux-shaped — there is no Linux syscall
for it (the nearest equivalent is a hand-assembled `isolcpus` +
`nohz_full` + `irqaffinity` + `SCHED_FIFO:99` + `mlockall` stack, and
even that is soft). It is a first-class NeoOS capability, reached
through `lib/` (no POSIX analogue → `lib/` wrapper, per `CLAUDE.md`),
and every property of it is recorded in `docs/stdlib.md` as a
deliberate divergence.

### Model

Two grades, selected by a flag:

- **`NEO_RT_DEDICATE_CPU`** — the caller's thread gets **one** CPU
  exclusively. The other CPUs keep running the normal scheduler and
  the rest of the system (init, the shell, other processes, kernel
  threads, timers, network) stays fully alive. This is the safe,
  composable grade — it is `isolcpus` done right and done at
  runtime.
- **`NEO_RT_TAKEOVER`** — the caller gets the **whole machine**. All
  other CPUs are parked in a quiescent loop (IPI'd to a
  `cli; hlt`-with-wakeword state), all other tasks are frozen, only
  the housekeeping needed to honour the deadman timer and to break
  out remains live. Maximum determinism, maximum blast radius. This
  is the grade the user asked for: "all other processes stop getting
  any CPU time and the CPU is totally allocated for one process."

### The syscalls (NeoOS-native numbers; `lib/` wrappers)

```c
// lib/neoos/rt.h
int  neo_rt_enter(unsigned grade, const struct neo_rt_attr *attr);
int  neo_rt_leave(void);
int  neo_rt_extend(uint64_t extra_ns);      // push the deadman out
int  neo_rt_status(struct neo_rt_status *out);

struct neo_rt_attr {
    uint64_t max_hold_ns;    // deadman: HARD cap, kernel-enforced,
                             // clamped to a build-time ceiling
                             // (default 1 s, absolute max e.g. 60 s)
    int      cpu;            // DEDICATE_CPU: which CPU (-1 = kernel picks
                             // an isolated one); ignored for TAKEOVER
    uint32_t flags;          // NEO_RT_KEEP_TIMERFD  -- leave the caller's
                             //   own timerfd/clock interrupts armed
                             // NEO_RT_KEEP_NET      -- keep the NIC IRQ
                             //   live (DEDICATE_CPU only)
                             // NEO_RT_LOCK_MEM      -- implied mlockall +
                             //   pre-fault + pin every page, no demand
                             //   paging, no COW while held
                             // NEO_RT_NO_SYSCALL    -- any syscall other
                             //   than neo_rt_leave/extend from the RT
                             //   thread => SIGSYS (forces the program to
                             //   be honest about its critical section)
};
```

Entering requires privilege (`CAP_SYS_NICE`-equivalent, i.e. uid 0 or
an explicit grant) — a NeoOS-native capability check, since a
non-privileged process must never be able to freeze the machine.

### What `neo_rt_enter` does

`NEO_RT_LOCK_MEM` first (always, for `TAKEOVER`): `mlockall(MCL_CURRENT
| MCL_FUTURE)` semantics + walk every VMA and fault/pin every page so
there is zero page-fault work possible while held. Fail the call if
the working set does not fit.

`DEDICATE_CPU`:
1. Pick / validate the target CPU. Prefer a CPU already at the leaf
   of an otherwise-idle domain.
2. `stop_sched_class`-style: migrate every migratable task off that
   CPU to its siblings (respecting affinities — a task pinned *only*
   to this CPU blocks the call with `-EBUSY` unless
   `NEO_RT_FORCE` is set, which parks it instead).
3. Re-route device IRQ affinity away from the CPU
   (`ioapic`/`lapic` redirection entries) except those the flags say
   to keep. The LAPIC timer on that CPU is disarmed (the tickless
   path from SCH-1, taken to its limit — no housekeeping tick at
   all).
4. Mark the rq `rt_dedicated`: `schedule()` on that CPU short-circuits
   to "run `curr` forever"; nothing is ever enqueued there; the
   balancer's domain masks exclude it (`cpu_isolated`).
5. Arm the **deadman**: a one-shot timer on a *housekeeping* CPU (not
   the dedicated one) for `max_hold_ns`. On expiry it IPIs the
   dedicated CPU with a high-priority vector that forcibly runs
   `neo_rt_leave()` on the RT thread's behalf and delivers `SIGXCPU`.
6. Return to userland. From here the thread runs with no preemption,
   no tick, no migration, no page faults — bounded worst-case
   interrupt latency = just the NMI/MCE/deadman vectors.

`NEO_RT_TAKEOVER` additionally:
7. Send every *other* online CPU a "quiesce" IPI. Each parks in
   `rt_takeover_park()`: save nothing, `cli`, spin-`hlt` on a
   per-CPU wakeword, servicing only NMI. Their current tasks are
   left `THREAD_RUNNING`-but-frozen (not requeued, not counted).
8. Freeze global timers, the network RX thread, `kswapd`-equivalent,
   everything. The only things still ticking anywhere are the
   deadman on... there is no other CPU — so for `TAKEOVER` the
   deadman is a **local LAPIC one-shot on the dedicated CPU itself**,
   at a vector the RT code cannot mask (it runs with `IF=1` but the
   handler is tiny and its only job is to force `neo_rt_leave`), plus
   a hardware fallback (see safety).

### `neo_rt_leave` / exit

- Explicit `neo_rt_leave()`: disarm the deadman, restore IRQ
  affinities, un-isolate the rq, for `TAKEOVER` wake every parked CPU
  (clear wakewords, re-arm their timers, thaw their tasks), rebuild
  the balancer domain masks, resume normal scheduling. The RT thread
  drops back to whatever policy it had before (`SCHED_NORMAL` by
  default).
- Process/thread **exit** while held: the exit path detects
  `rt_dedicated`/`rt_takeover` and runs the same teardown before
  reaping. A crash (SIGSEGV etc.) in the RT thread → the fault
  handler runs the teardown, *then* delivers the signal / kills the
  process. The machine must never be left frozen by a bug in the RT
  program — this is the single most important correctness property.
- `neo_rt_extend(extra_ns)`: re-arms the deadman further out, up to
  the absolute ceiling. Lets a well-behaved long job hold the CPU
  without picking an unsafe `max_hold_ns` up front.

### Safety (non-negotiable)

- **Deadman timer is mandatory and kernel-clamped.** No "hold
  forever" mode. The build-time absolute ceiling exists so a
  compromised or buggy privileged process still cannot permanently
  wedge the box.
- **Hardware watchdog fallback.** If the platform exposes one
  (ACPI WDAT, or the Intel TCO watchdog, or QEMU's `-watchdog`),
  arm it at `max_hold_ns + slack` before `TAKEOVER` and pet it from
  the deadman handler. If the RT CPU is truly wedged (deadman vector
  masked by a bug, infinite loop with `cli`), the watchdog resets
  the machine — a reboot beats a brick.
- **NMI escape.** The kernel keeps an NMI handler that checks a
  "break RT now" flag (settable from a serial-console magic-sysrq
  equivalent, or a GPIO / IPMI path on real hardware). `TAKEOVER`
  cannot mask NMIs.
- **No `TAKEOVER` before SMP is proven quiescent-park-able.** The
  quiesce/thaw of other CPUs reuses the `smp_panic_stop_others`
  machinery (already exists, already selftested —
  `[smp] panic-stop selftest passed`) but in a *reversible* form;
  that reversibility gets its own selftest before `TAKEOVER` ships.
- **Refuse if the caller holds resources others need**: an open
  pipe/socket with a blocked peer, a held file lock, membership in a
  cgroup with `cpu.max` — `TAKEOVER` with `NEO_RT_STRICT` fails the
  call rather than deadlocking the frozen rest-of-system.

### Interaction with the rest of the scheduler

- Sits **above `dl_sched_class`** in dispatch on the dedicated CPU —
  but really it bypasses class dispatch entirely (`schedule()` early
  return).
- `DEDICATE_CPU` composes with SCH-2/6: the balancer already has a
  `cpu_isolated` mask concept (from `isolcpus`); dedication just sets
  a bit in it dynamically and clears it on leave.
- Deliberately **incompatible** with `SCHED_DEADLINE` admission on
  the same CPU (the CPU's bandwidth is 100% committed) — admission
  control rejects new dl tasks whose affinity includes a dedicated
  CPU.
- A `SCHED_FIFO:99` task is the "soft" version and remains available
  for programs that need low latency but must stay co-operative;
  `docs/stdlib.md` documents the ladder: `SCHED_RR` <
  `SCHED_FIFO` < `SCHED_DEADLINE` < `NEO_RT_DEDICATE_CPU` <
  `NEO_RT_TAKEOVER`, with the latency/isolation and the blast-radius
  both increasing down the list.

### `docs/stdlib.md` entry (divergence)

> **`neo_rt_enter`/`neo_rt_leave`/`neo_rt_extend`/`neo_rt_status`** —
> NeoOS-native, no POSIX/Linux equivalent. Converts the scheduler on
> one CPU (`DEDICATE_CPU`) or the whole machine (`TAKEOVER`) into a
> hard-realtime, no-preemption, no-tick mode dedicated to a single
> thread. Privileged. A mandatory kernel-clamped deadman timer bounds
> the hold; process crash/exit auto-releases; a hardware watchdog is
> the last-resort escape. `TAKEOVER` freezes every other process and
> CPU. Use `SCHED_FIFO`/`SCHED_DEADLINE` for co-operative low latency;
> use this only for a genuinely uninterruptible critical section.

### Testing

- `DEDICATE_CPU` on `-smp 4`: enter on CPU 3 with 8 CPU-bound tasks
  running → CPU 3 runs *only* the RT thread (measured: zero other
  threads scheduled there for the hold), the other 7 tasks pack onto
  CPUs 0–2, the shell on CPU 0 stays responsive.
- RT thread busy-loops reading `rdtsc` → inter-iteration jitter
  under the hold is bounded by NMI/deadman only (target: p99.99
  < 1 µs on TCG, near-zero on KVM).
- Deadman: `max_hold_ns = 100 ms`, RT thread `while(1)` → at 100 ms
  the machine is forcibly returned, `SIGXCPU` delivered, all tasks
  resume, gauntlet-style markers all still fire afterward.
- Crash-safety: RT thread does `*(int*)0 = 1` mid-hold → teardown
  runs, SIGSEGV delivered, machine fully recovers.
- `TAKEOVER`: enter, spin 50 ms, leave → serial log shows every
  other CPU parked then thawed, `[timer] tick` gap exactly the hold
  duration on the non-dedicated CPUs, no task lost, no lock left
  held, subsequent full gauntlet passes.
- Negative: non-privileged `neo_rt_enter` → `-EPERM`.
  `max_hold_ns` above the ceiling → clamped, `neo_rt_status` reports
  the clamp.

### os-builder / port note

Per `CLAUDE.md`'s "Keeping neoos-os-builder in sync": this adds a
`lib/` surface, so `docs/stdlib.md` must document it and any port
that uses it (a motion-control / DAQ / SDR port is the obvious
consumer) needs `neoos-os-builder`'s `build.sh` to know it links
`libneoos`. Fold into whichever port milestone first needs it.

---

## SCH-8 — Core scheduling (SMT)

**Scope**: lowest priority; only meaningful if NeoOS runs on SMT
hardware / QEMU `-smp ...,threads=2` and cross-thread interference
or side-channel isolation matters.

- Per-task `core_cookie`; a physical core only co-schedules siblings
  with matching cookies, forcing a sibling idle otherwise.
- `prctl(PR_SCHED_CORE)` ABI.
- The core-wide `pick_next` (`pick_next_task` picks for the whole
  core at once, comparing across siblings' runqueues).

Likely stays a stub / "documented not-implemented" unless a concrete
need appears.

---

## Cross-cutting: observability

Ships incrementally with the milestones:

- `/proc/schedstat`, `/proc/[pid]/sched`, `/proc/[pid]/stat` fields
  9–24 (priority, nice, num_threads, vsize…), `/proc/[pid]/status`
  (`Cpus_allowed`, `voluntary_ctxt_switches`).
- `/sys/kernel/debug/sched/` (`base_slice_ns`, `migration_cost_ns`,
  domain dumps) — behind a debug build.
- A boot-time `[sched]` selftest block (the `[smp] … selftest
  passed` model) asserting the invariants of whichever milestones
  are compiled in.
- `perf sched`-style tracepoints are out of scope until NeoOS has a
  tracing framework.

## Cross-cutting: lock ranks

New ranks needed (slot them per `kernel/sync/lock.h`'s existing
discipline, renumbering the block above as SCH work lands — same
mechanical shift already done once this session for
`LOCK_RANK_EPOLL_LIST`):

- `LOCK_RANK_RQ` (was `LOCK_RANK_RUNQUEUE`; the double-rq helper
  already handles equal-rank pairs by address order)
- `LOCK_RANK_CFS_BANDWIDTH`, `LOCK_RANK_DL_BANDWIDTH`
- `LOCK_RANK_TASK_GROUP` (the task_group tree; above rq)
- `LOCK_RANK_SCHED_DOMAIN` (domain rebuild, CPU hotplug; a slow-path
  mutex-like lock, well above rq)
- `LOCK_RANK_NUMA` for `pglist_data` (mm side; ordered with
  `LOCK_RANK_PMM`)

## Sequencing & dependencies

```
SCH-1 EEVDF core ──┬─> SCH-2 SMP balancer ──┬─> SCH-3 RT class ──> SCH-4 DEADLINE
                   │                        ├─> SCH-6 NUMA (needs domains)
                   │                        └─> SCH-9 RT takeover (needs isolation + tickless)
                   └─> SCH-5 group sched (needs EEVDF recursion)
SCH-2 ──> SCH-7 PELT/capacity/EAS (upgrades SCH-2's load metric)
SCH-8 core sched — independent, last, maybe never
```

Recommended order: **SCH-1 → SCH-2 → SCH-3 → SCH-9 → SCH-5 → SCH-6 →
SCH-4 → SCH-7 → SCH-8**. SCH-9 (the NeoOS hard-realtime takeover) is
pulled early — right after the RT class — because it is the
headline NeoOS-native feature, it only needs SCH-1's tickless one-shot
timer + SCH-2's `cpu_isolated` mask + the existing
`smp_panic_stop_others` machinery, and it does not depend on group
scheduling, NUMA, or DEADLINE. SCH-3 before SCH-5 because RT is simpler and shakes
out the class-dispatch refactor; SCH-6 before SCH-4 because NUMA
touches mm broadly and DEADLINE is self-contained. Each is a shippable
increment with its own gauntlet pass.

## Risks / open questions

- **The concurrent-request crash** (`docs/aspnet-missing-syscalls.md`)
  is likely an mm-under-SMP race. Per the chosen sequencing the
  scheduler work comes first — SCH-1/SCH-2 will churn the same hot
  paths, so either the crash gets fixed incidentally or it must be
  root-caused *before* SCH-2 so the balancer isn't built on a broken
  invariant. The concurrent-request spec treats "reproduce with a
  pure-C pthread stress test, bisect against `-smp 1`" as a
  prerequisite check-item for SCH-2.
- Tickless (`NO_HZ_FULL`) is deliberately out of scope — huge, and
  the one-shot timer from SCH-1 gets most of the benefit.
- `lib/rbtree.c` is the critical shared dependency; it must land
  first, standalone, with its selftest green, or three milestones
  are blocked on a buggy tree.
- Rebuilding sched domains on CPU hotplug — NeoOS has no CPU hotplug
  today, so domains are built once at SMP bringup. Keep the rebuild
  path a function even so; it's needed for suspend/resume later.
