# SMP object lifetime and the proc_lock / sig_lock detangle — design

**Date:** 2026-08-31
**Status:** design, approved in chat; ready for implementation plan
**Predecessor:** `docs/superpowers/specs/2026-08-31-smp-hardening-handoff.md`
and its "Phase 13.5" write-up in `docs/optimization-summary.md`.

## Problem

Landing Phase 13 and grinding a 15-run `make test` gauntlet on 4 CPUs
fixed four SMP races (tlb.c deferred-free, serial interleaving,
thread-list under-locking, stack-slot bitmap). The gauntlet is now
15/15 green. Three problems were found that could **not** be fixed as
part of that pass because they share one architectural knot:

1. **The per-process thread list is still read without its guard in
   `kernel/ipc/signal.c`.** `signal_send_process` walks `p->threads`
   with no lock, picks a `target` thread, releases nothing, and calls
   `signal_wake_for_delivery(target)` — a use-after-free against a
   concurrent `thread_join` that frees that thread. `signal_do_stop`
   and `signal_do_continue` have the same shape.

2. **It is not a mechanical lock-add.** `signal_send_process` /
   `signal_send_thread` are *designed* to be called with the global
   `proc_lock` already held: `signal_kill` / `signal_tkill` hold it
   across the whole delivery loop for process-group signal ordering
   (see the comment at `proc.c:654`, "never reverse it"). A plain
   `spin_lock_irqsave(&proc_lock)` inside the senders self-deadlocks.
   The `proc_table` per-bucket locks also share rank
   `LOCK_RANK_PROCTABLE` with `proc_lock`, so an iterator cannot take a
   bucket lock while `proc_lock` is held. Plan Task 6 (drop
   `proc_list`) sits on the same knot.

3. **RCU is inert.** `rcu_init()` and `synchronize_rcu()` are never
   called anywhere, so the `call_rcu` callbacks the Phase 3/4 hash
   tables queue (`thread_rcu_free`, `proc_rcu_free`) never run.
   `proc_reap`'s "freed via RCU callback" comment is wrong: the legacy
   `p->zombies` / `proc_list` paths are what actually free, and
   processes leak on exit (the `proc_rcu_free` that would `pid_free` +
   `kfree` never fires).

The root cause common to all three: **"find an object in a table, drop
the table lock, then operate on the object" is unsafe** (the object can
be freed underneath you), and the codebase's answer so far — hold a
single global lock across the whole operation — forces every
signal-send to be callable under that lock and rank-collides with the
table's own bucket locks.

## Goals

- The per-process thread list has exactly one guard, held at every
  read and every write, including the `signal.c` walks.
- A `struct thread *` or `struct process *` obtained from a table stays
  valid for as long as its holder needs it, without holding a lock.
- No lock is held across signal delivery.
- `proc_list` and the global `proc_lock` are gone.
- Processes and threads are freed exactly once, when nothing references
  them.
- `make test` gauntlet stays 15/15 green after every commit.

## Non-goals

- Removing the per-process lock's coarseness (it guards signal state
  *and* thread membership — acceptable at 4 CPUs).
- Making `thread_table` the primary thread lookup; the `proc_next`
  linked list stays as the enumeration structure.
- A real SMP RCU. RCU is deleted, not fixed.
- The `[timer] tick=` cosmetic interleave; block-cache write-through.

## Design

### 1. Object lifetime: reference counts

**`struct thread` gains `uint32_t ref`.**

- `thread_alloc` sets `ref = 1` — the reference held by the process's
  thread list.
- `thread_get(t)` increments; `thread_put(t)` decrements and, at zero,
  frees `t->xstate` and `t`. The **kernel stack** is still freed
  earlier and separately, by whoever removes `t` from the zombie list,
  *after* `thread_wait_off_cpu(t)` — unchanged.
- live→zombie (in `thread_exit_self`) does not touch `ref`: the thread
  is still on a list.
- Removing `t` from the zombie list (`thread_join`, `proc_reap`, the
  idle-thread `kzombies` drain) transfers that one reference to the
  remover, which does `thread_wait_off_cpu` → free stack →
  `thread_put`.
- A transient holder (signal delivery selecting a `target`,
  `process_exit` snapshotting siblings) takes `thread_get` under
  `p->lock` and `thread_put` when done. This is what closes the
  `process_exit` snapshot UAF noted in Phase 13.5.

**`struct process` gains `uint32_t ref`, distinct from the renamed
`live_threads`.**

- `ref = 1` at creation — the `proc_table`'s reference.
- `proc_get(p)` / `proc_put(p)`; `proc_put` at zero does `pid_free` +
  `kfree(p)`.
- `proc_table_remove` drops the table's reference.
- Any table lookup whose result outlives the bucket lock returns the
  pointer **ref'd**; the caller does `proc_put`.
- The old `proc_put` — "last live thread out frees the address space
  and turns the process into a zombie" — is renamed **`proc_put_live`**
  and continues to own that logic, decrementing `live_threads`. It is
  orthogonal to `ref`.
- `proc_reap` frees the zombie threads and the per-process tables, then
  calls `proc_table_remove` + `proc_put`.

### 2. The per-process lock

`p->sig_lock` is renamed **`p->lock`** (rank unchanged,
`LOCK_RANK_PROCESS` = 1). It now guards:

- signal dispositions, `pending`, `queued`, `stopping`, `altstack`
  (as today), **and**
- `p->threads`, `p->zombies`, `p->live_threads` (moved off the global
  `proc_lock` that Phase 13.5's fix #3 put them under).

`p->refcount` is renamed **`p->live_threads`** — it is a count of
running threads, not a reference count, and the collision with the new
`p->ref` is a trap.

Thread-list mutation/scan sites move from `proc_lock` to `p->lock`:

| Site | Under `p->lock` | Outside |
|---|---|---|
| `thread_alloc` | list insert, `live_threads++` (tid computed **before** — `alloc_id()` is rank `PROCTABLE` 0, below `p->lock` 1) | `kmalloc`s |
| `thread_exit_self` | live→zombie move only | `proc_put_live`, `waitq_wake_all`, `schedule` |
| `thread_join` | both list scans; unlink + transfer ref; `waitq_sleep(&p->join_waiters, &p->lock)` | `thread_wait_off_cpu`, stack free, `thread_put`, `thread_stack_free` |
| `proc_reap` | detach `p->zombies` head | walk + free |
| `process_exit` | snapshot siblings + `thread_get` each | SIGKILL each, `thread_put` each |

**`kzombies`** (kernel-thread zombie list) leaves `proc_lock` for a
dedicated `kzombies_lock`, rank `LOCK_RANK_PROCTABLE`.
`thread_exit_self`'s kernel-thread branch and `idle_entry`'s drain hold
nothing else when touching it.

### 3. The signal-send path

Each sender splits:

```
signal_send_thread(t, sig, info):
  signal_stop_cont_interlock(p, sig)   # may take p->lock; caller holds nothing
  if signal_is_ignored(p, sig): return 0
  f = spin_lock_irqsave(&p->lock)
  rc = signal_send_thread_locked(t, sig, info)
  spin_unlock_irqrestore(&p->lock, f)
  return rc

signal_send_process(...): same shape, calls signal_send_process_locked
```

- The `_locked` variants assume `p->lock` held. They do
  `queue_append` / `sigset_add`, the sibling walk, and
  `signal_wake_for_delivery` **all under `p->lock`**. The wake path is
  deadlock-safe: `thread_wake(t, from)` returns immediately unless
  `t->state == from`, and a thread in `thread_exit_self` is `ZOMBIE`,
  so `wait_off_cpu` is never entered for it; a genuinely parked thread
  clears `on_cpu` by finishing a context switch, which needs no lock.
- `signal_stop_cont_interlock` (→ `signal_do_continue` / the stop
  path) takes `p->lock` itself and is called **only** from the public
  wrappers, before `p->lock` is acquired — no recursion.
- `signal_do_stop`'s `all_stopped` walk and `signal_do_continue`'s two
  walks move under `p->lock`. `signal_do_continue`'s documented
  lost-wakeup handling already runs under this lock; the change widens
  the critical section, it does not reorder the state/lock handoff.
- **`signal_tkill`**: iterate to the target process (ref'd),
  `spin_lock_irqsave(&p->lock)`, walk `p->threads` for `tid`,
  `signal_send_thread_locked`, unlock, `proc_put`. The recursion into
  `signal_send_thread` is gone.
- **`signal_kill`** (group): the streaming iterator (§4) yields each
  ref'd process with no lock held; for each, call the public
  `signal_send_process`, then `proc_put`. No lock spans the loop, so
  the "`proc_lock` held across `sig_lock`" constraint at `proc.c:654`
  disappears with its comment.

### 4. proc_table: per-bucket locks, refcount, streaming iterator

Buckets keep their per-bucket `spinlock` at rank `LOCK_RANK_PROCTABLE`.

- `proc_table_lookup(pid)` → take bucket lock, find, `proc_get(p)`,
  drop bucket lock, return `p` ref'd (or NULL). `proc_find` forwards
  this contract; every caller does `proc_put`.
- `proc_table_insert` / `proc_table_remove` — bucket-locked as now;
  `remove` ends with `proc_put` in place of `call_rcu`.
- New:
  ```c
  // Runs fn(p, ctx) for every live process, with a ref held on p and
  // NO lock held. fn must not take a bucket lock or insert/remove.
  void proc_table_for_each_ref(void (*fn)(struct process *, void *),
                               void *ctx);
  ```
  Per bucket, repeat until the bucket yields nothing new: take the
  bucket lock, scan the chain from the head, and for each process not
  yet stamped with the current iteration's generation, stamp it,
  `proc_get` it, and copy it into a `K = 8` buffer — stop the scan when
  the buffer is full; drop the lock; call `fn` + `proc_put` for each
  buffered process. Resuming by a generation stamp on `struct process`
  (a `uint32_t`, compared against a global iteration counter, no
  clearing needed) rather than by index tolerates the chain shifting
  between passes. A process removed mid-iteration is either still ref'd
  for this pass (`fn` re-checks `p->state`) or was never stamped and is
  skipped.

**Consumers converted** to `proc_table_for_each_ref` + a ctx struct:
`signal_kill` group path, `signal_tkill`, `wait4`'s child scan.

**`proc_list` deleted**: the `struct process *proc_list` decl, the
`extern` in `sched.h`, the insert in `proc_alloc` (`proc.c:87-91`), the
remove in `proc_reap` (`proc.c:808-813`), the `p->next` member.

### 5. RCU removal

- Delete `kernel/sync/rcu.c` and `kernel/sync/rcu.h`.
- Remove `struct rcu_head rcu` from `struct process` and
  `struct thread`; remove `#include "sync/rcu.h"` sites.
- `thread_table_remove`: drop the `call_rcu(&t->rcu, thread_rcu_free)`
  — the thread-list reference (§1) owns the free; the bucket unlink is
  all that remains here.
- `proc_table_remove`: `call_rcu(&proc->rcu, proc_rcu_free)` →
  `proc_put(proc)`.
- `rcu_dereference` / `rcu_assign_pointer` in `proc_table_lookup` /
  `thread_table_lookup` → plain `__atomic_load_n` /
  `__atomic_store_n` with `__ATOMIC_ACQUIRE` / `__ATOMIC_RELEASE`
  (the lookups are bucket-locked now; the barrier is belt-and-braces).
- `thread_table_lookup` returns a ref'd pointer for consistency, even
  though nothing outside the process holds it long today.

## Implementation slices

Each commit builds and is gauntlet-green (15/15) before the next.
Prefix `SMP hardening:` (continuing Phase 13.5) or open `Phase 14:` per
the milestone's framing at the time.

1. **Renames only.** `sig_lock`→`lock`, `refcount`→`live_threads`, add
   `kzombies_lock` and move the `kzombies` list onto it. No behaviour
   change. Gauntlet.
2. **Thread-list guard → `p->lock`.** Re-point the five fix-#3 sites
   from `proc_lock` to `p->lock`; re-verify each care point. Gauntlet.
3. **Thread refcount.** `t->ref`, `thread_get` / `thread_put`; wire
   through `thread_alloc` / `thread_join` / `proc_reap` /
   `thread_exit_self` / idle drain / `process_exit` snapshot. Gauntlet.
4. **`_locked` signal senders.** Split `signal_send_thread` /
   `signal_send_process`; move the `signal.c` walks under `p->lock`;
   fix the `signal_tkill` recursion. Gauntlet.
5. **Process refcount.** `p->ref`, `proc_get` / `proc_put`; rename old
   `proc_put`→`proc_put_live`; `proc_table_lookup` / `proc_find`
   return ref'd pointers; audit and fix every `proc_find` caller
   (starting list below). Gauntlet.
6. **Streaming iterator + drop `proc_list`.** Add
   `proc_table_for_each_ref`; convert `signal_kill`, `signal_tkill`,
   `wait4`; delete `proc_list` and `p->next`. Gauntlet.
7. **Delete RCU.** Remove `rcu.{c,h}`, `rcu_head` members, `call_rcu`
   from the tables; lookups use plain atomics. Gauntlet ×3.
8. **Docs.** `optimization-summary.md` (close the Phase 13.5 residual
   list, add the milestone entry), `abi-compatibility.md` refresh
   (no ABI surface changed — record that), handoff + SDD ledger
   close-out. `docs/stdlib.md` unchanged (no user-visible behaviour
   change; `thread_join` / `kill` semantics are the same, only correct
   under contention).

### `proc_find` / `proc_table_lookup` callers to audit in slice 5

| Site | Use | Likely disposition |
|---|---|---|
| `proc.c` `wait_for_pid` (`:~893`) | sleeps on `p->exit_waiters`, then `proc_reap(p)` | ref'd; `proc_put` after reap |
| `proc.c` `proc_put_live` SIGCHLD to parent (`:~741`) | synchronous `signal_send_process(parent,…)` | ref'd; `proc_put` after send |
| `proc.c` `signal_kill` single-pid path (`:~663`) | synchronous send | ref'd; `proc_put` after send |
| `signal.c` `signal_do_stop` parent wake (`:329`) | `waitq_wake_all(&parent->child_waiters)` | ref'd; `proc_put` after |
| `sys_proc.c` getpgid/setpgid/getsid (`:115,122,135`) | read `pgid`/`sid` under no lock | ref'd; `proc_put` before return |
| `current_proc()` everywhere | the running thread's own process | borrowed — never freed under you, no ref |

## Testing

- **Gauntlet** (`.superpowers/sdd/2026-08-31-phase14-input-and-solidity/gauntlet.sh`)
  is the regression bar for the timing races — 15/15 after every slice,
  ×3 after slice 7.
- **Deterministic where possible:** `userland/fork_test.c` gains the
  PID-visibility assertion from the Task 6 brief (child findable by
  `kill(pid,0)` the instant `fork` returns, gone the instant it is
  reaped). A new kernel selftest hammers concurrent
  `thread_create` / `thread_join` / `tkill` on one multi-threaded
  process to exercise the refcount and `_locked` paths under a
  known-heavy load; it asserts no leak (thread count returns to
  baseline) and no `-ESRCH` on a live tid.
- `smptest` already covers the 8-threads-exit-at-once join path.
- A rank-inversion from any of the lock moves panics at boot
  (`[lock] PANIC`), caught immediately.

## Risks

1. **`proc_find` caller audit is wide.** A missed `proc_put` leaks; a
   spurious one is a use-after-free. Mitigation: slice 5 does nothing
   else, and the caller list above is the checklist.
2. **`signal_do_stop` / `signal_do_continue` lost-wakeup history.**
   Slice 4 widens their critical section under the same lock; it must
   not reorder the `THREAD_STOPPED` publish vs. the `stopping` clear.
3. **Refcount / `thread_wait_off_cpu` ordering.** The kernel-stack free
   must stay *after* `thread_wait_off_cpu(t)` and *before* the final
   `thread_put(t)`, at every one of the three drain sites.
4. **`K = 8` streaming buffer.** A bucket with a burst of processes
   forces multiple passes; the generation-stamp resume (§4) is the
   correctness-critical part — an index cursor would double-visit or
   skip when the chain shifts. The global iteration counter must be
   taken under a small lock (or atomically bumped) so two concurrent
   iterations get distinct generations.
