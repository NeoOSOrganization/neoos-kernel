# SMP Object Lifetime and the proc_lock / sig_lock Detangle — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give `struct thread` and `struct process` reference counts, make
the per-process lock (`sig_lock` → `p->lock`) the sole guard of the
thread lists including the `signal.c` walks, split the signal-send path
into `_locked` variants, replace the global `proc_lock` + `proc_list`
with a streaming ref'd `proc_table` iterator, and delete the inert RCU.

**Architecture:** The knot is "find an object in a table, drop the table
lock, use the pointer" — unsafe without a refcount, and today worked
around by holding one global lock across the whole operation, which
forces every signal-send to be callable under that lock and rank-collides
with the table's bucket locks. We add refcounts so a looked-up pointer
stays valid with no lock held; move thread-list membership onto the
existing per-process lock so the signal walks are correct; give the
senders `_locked` inner functions so callers that already hold the lock
don't deadlock; and iterate the process table by streaming small batches
of ref'd pointers with no lock held during the callback.

**Tech Stack:** C (freestanding, `-ffreestanding -mcmodel=large`), NASM,
x86-64, GNU Make, headless QEMU + serial-log assertions. No host test
runner — every test is a kernel selftest (`serial_write_string`) or a
userland suite whose `[name] ALL PASSED` line is a `REQUIRED_MARKER` in
the `Makefile`. The 15-run gauntlet is the regression bar for timing
races.

**Spec:** `docs/superpowers/specs/2026-08-31-smp-lifetime-and-lock-detangle-design.md`

## Global Constraints

- **No host unit tests.** Each task's "test" is a kernel selftest or a
  userland test binary, verified by `make test` (headless QEMU, 4 CPUs,
  COM1 → `build/serial.log`). "See it fail" means: apply only the test,
  run `make test`, confirm a `FAILED` line or a missing `REQUIRED_MARKER`.
- **The gauntlet is the race regression bar.** After every task:
  `bash .superpowers/sdd/2026-08-31-phase14-input-and-solidity/gauntlet.sh`
  must reach `GAUNTLET PASSED: 15/15`. Run it ×3 after Task 7. A single
  `make test` run is NOT sufficient sign-off for any task in this plan.
- **Lock rank order is enforced at runtime.** A new lock gets a rank in
  the `LOCK_RANK_*` enum (`kernel/sync/lock.h`); acquiring out of rank
  order (equal ranks included) panics the boot with `[lock] PANIC`.
  This plan adds exactly one rank use (`kzombies_lock` at the existing
  `LOCK_RANK_PROCTABLE`) and removes one lock (`proc_lock`). Do not
  change any other rank or acquisition order.
- **Kernel includes are relative to `kernel/`** (`#include "sched/foo.h"`),
  made to work by `-Ikernel`.
- **Work happens directly on `main`.** One commit per task. End every
  commit message with:
  ```
  Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_0137pJDZxcY3aShoE1xM93MF
  ```
- **Regenerate disk images between runs:** the gauntlet script already
  does `rm -f build/disk.img build/disk2.img`; for a bare `make test`,
  do it yourself.
- **Never run `make build` / `make test` while the gauntlet is running**
  — they share `build/` and will corrupt each other. Serialize.
- **No ABI surface changes.** `thread_join` / `kill` / `wait4` semantics
  are unchanged; they only become correct under contention.
  `docs/stdlib.md` is not touched.

---

## File Structure

No new source files. Modified:

| File | Responsibility in this plan |
|---|---|
| `kernel/sched/proc.h` | `struct process` / `struct thread` gain `ref`; `refcount`→`live_threads`; `sig_lock`→`lock`; drop `p->next`, `rcu_head`; declare `thread_get/put`, `proc_get/put`, `proc_put_live` |
| `kernel/sched/proc.c` | thread-list sites move to `p->lock`; refcount impls; `proc_put`→`proc_put_live`; `proc_find` returns ref'd; `proc_reap` / `wait4` rework; drop `proc_list` |
| `kernel/sched/thread.c` | `thread_alloc` / `thread_exit_self` / `thread_join` under `p->lock` + refcount; `kzombies_lock` |
| `kernel/sched/sched.c` | `idle_entry` kzombies drain under `kzombies_lock`; `kzombies` decl |
| `kernel/sched/sched.h` | drop `proc_list` / `proc_lock` externs; add `kzombies_lock` extern |
| `kernel/ipc/signal.c` | `_locked` senders; walks under `p->lock`; `signal_tkill` rework; `proc_find` ref'd |
| `kernel/sched/proc_table.c` / `.h` | bucket-locked ref'd lookup; `proc_table_for_each_ref`; drop `call_rcu`; generation stamp |
| `kernel/sched/thread_table.c` / `.h` | drop `call_rcu`; ref'd lookup; plain atomics |
| `kernel/syscall/sys_proc.c` | `proc_find` callers do `proc_put` |
| `kernel/syscall/sys_signal.c` | `p->sig_lock` → `p->lock` rename |
| `kernel/sync/rcu.c` / `.h` | **deleted** (Task 7) |
| `kernel/kernel.c` (or wherever selftests are called) | register the new concurrency selftest |
| `Makefile` | drop `rcu.o`; add the selftest source if separate; `REQUIRED_MARKER` for the selftest |
| `userland/fork_test.c` | PID-visibility assertion (Task 6) |

---

## Task 1: Renames — `sig_lock` → `p->lock`, `refcount` → `live_threads`, `kzombies_lock`

Pure mechanical rename plus one lock swap. No behaviour change. This
task exists on its own so the later diffs are readable.

**Files:**
- Modify: `kernel/sched/proc.h`, `kernel/sched/proc.c`,
  `kernel/sched/thread.c`, `kernel/sched/sched.c`,
  `kernel/sched/sched.h`, `kernel/ipc/signal.c`,
  `kernel/syscall/sys_signal.c`, `kernel/dev/tty.c` (comment only)

**Interfaces:**
- Produces: `struct process` field `lock` (was `sig_lock`), field
  `live_threads` (was `refcount`); global `struct spinlock kzombies_lock`
  declared in `sched.h`, defined in `sched.c`.

- [ ] **Step 1: rename `sig_lock` → `lock`**

  In `kernel/sched/proc.h`, rename the `struct spinlock sig_lock;` member
  of `struct process` to `struct spinlock lock;`. Update the comment
  above it to: `// The per-process lock. Guards signal dispositions,`
  `// pending/queued sets and stop state, AND thread membership`
  `// (threads, zombies, live_threads). Rank LOCK_RANK_PROCESS.`

  Rename every `->sig_lock` to `->lock` and `.sig_lock` to `.lock`
  across: `kernel/ipc/signal.c` (incl. the `spin_init(&p->sig_lock,
  LOCK_RANK_PROCESS, "signal")` at line ~26 — keep the name string
  `"proc"` now), `kernel/syscall/sys_signal.c` (lines ~35, ~41).

  Grep to confirm none remain: `grep -rn 'sig_lock' kernel/` returns
  only the stale comment in `kernel/dev/tty.c:164` — update that comment
  to say `p->lock` instead of `sig_lock`.

- [ ] **Step 2: rename `refcount` → `live_threads`**

  In `kernel/sched/proc.h`, rename `uint32_t refcount;` to
  `uint32_t live_threads;`. Keep/adjust the comment: `// One count per`
  `// LIVE thread. When it reaches zero the address space is freed and`
  `// the process becomes a zombie; the struct outlives that and is`
  `// freed by its last reference (see proc_put).`

  Rename every `->refcount` (grep: `proc.c` `proc_get`/`proc_put`/
  `proc_alloc`/`fork`, `thread.c` `thread_alloc`). `grep -rn 'refcount'
  kernel/` must return nothing after.

- [ ] **Step 3: add `kzombies_lock`, move the kzombies list onto it**

  In `kernel/sched/sched.h`, next to `extern struct thread *kzombies;`
  add `extern struct spinlock kzombies_lock;`.

  In `kernel/sched/sched.c`, next to `struct thread *kzombies;` add
  `struct spinlock kzombies_lock;`. In `process_init()` in `proc.c`
  (right after `spin_init(&proc_lock, ...)` — which still exists until
  Task 6) add:
  `spin_init(&kzombies_lock, LOCK_RANK_PROCTABLE, "kzombies");`

  In `kernel/sched/thread.c` `thread_exit_self`, the `else` (kernel
  thread) branch currently does `spin_lock_irqsave(&proc_lock)` around
  the `kzombies` push — change that to `&kzombies_lock`.

  In `kernel/sched/sched.c` `idle_entry`, the drain currently does
  `spin_lock_irqsave(&proc_lock)` around reading+nulling `kzombies` —
  change to `&kzombies_lock`.

- [ ] **Step 4: build and full-suite check**

  Run: `rm -f build/disk.img build/disk2.img && make test 2>&1 | tail -5`
  Expected: `PASS: ... all suites reported`, exit 0. No `[lock] PANIC`
  (a rank mistake in the kzombies swap would panic here).

- [ ] **Step 5: gauntlet**

  Run: `bash .superpowers/sdd/2026-08-31-phase14-input-and-solidity/gauntlet.sh`
  Expected: `GAUNTLET PASSED: 15/15`.

- [ ] **Step 6: commit**

  ```bash
  git add kernel/
  git commit -m "SMP hardening: rename sig_lock->p->lock, refcount->live_threads, add kzombies_lock

  Mechanical. p->lock now names the per-process lock that will also
  guard thread membership; live_threads names the running-thread count
  so it does not collide with the reference count added next. The
  kernel-thread zombie list moves off proc_lock onto its own
  kzombies_lock so proc_lock can be deleted later.

  <trailer>"
  ```

---

## Task 2: Thread-list guard moves from `proc_lock` to `p->lock`

Re-point the five sites Phase 13.5's fix #3 put under `proc_lock`
(commit `04e4866`) to `p->lock`. Same structure, per-process lock.

**Files:**
- Modify: `kernel/sched/thread.c` (`thread_alloc`, `thread_exit_self`,
  `thread_join`), `kernel/sched/proc.c` (`proc_reap`, `process_exit`)

**Interfaces:**
- Consumes: `struct process` field `lock` (Task 1).
- Produces: `p->threads` / `p->zombies` / `p->live_threads` guarded by
  `p->lock` at every site.

- [ ] **Step 1: `thread_alloc`**

  In `kernel/sched/thread.c` `thread_alloc`, the `if (p) { ... }` block
  currently takes `spin_lock_irqsave(&proc_lock)`. Change to
  `spin_lock_irqsave(&p->lock)`. Leave the tid computation where it is
  (BEFORE the lock) — `alloc_id()` reaches the pid allocator at rank
  `LOCK_RANK_PROCTABLE` (0), which is below `p->lock` (rank
  `LOCK_RANK_PROCESS` = 1); calling it under `p->lock` would be a
  descending acquire and panic. Update the comment that mentions
  "proc_lock (rank LOCK_RANK_PROCTABLE, the outermost)" to name
  `p->lock` and rank `LOCK_RANK_PROCESS`.

- [ ] **Step 2: `thread_exit_self`**

  Same file, the `if (p)` branch: `spin_lock_irqsave(&proc_lock)` around
  the live→zombie move → `&p->lock`. The scoping rule is unchanged and
  still load-bearing: the lock covers ONLY the list move, never
  `proc_put` (renamed `proc_put_live` in Task 5 — for now it is still
  `proc_put`), `waitq_wake_all`, or `schedule`. Keep the comment about
  the deadlock (a waker's `thread_wait_off_cpu` could spin on a sibling
  blocked here) — update "proc_lock" → "p->lock" in it.

  Note: the `p->thread_table` `thread_table_remove(p->thread_table, t)`
  call stays BEFORE the `p->lock` acquire, as now.

- [ ] **Step 3: `thread_join`**

  Same file. Every `spin_lock_irqsave(&proc_lock)` / `spin_unlock_
  irqrestore(&proc_lock, f)` / `waitq_sleep(&p->join_waiters,
  &proc_lock)` → `&p->lock`. Structure is otherwise unchanged: both
  scans under `p->lock`; on found-zombie, unlink + copy `exit_code`,
  release, then `thread_wait_off_cpu` / `thread_stack_free` / frees; on
  still-running, `waitq_sleep(&p->join_waiters, &p->lock)` then release
  then check `-EINTR`.

- [ ] **Step 4: `proc_reap`**

  In `kernel/sched/proc.c` `proc_reap`, the `spin_lock_irqsave(&proc_lock)`
  that detaches `p->zombies` → `&p->lock`.

- [ ] **Step 5: `process_exit`**

  Same file. The sibling snapshot loop currently takes
  `spin_lock_irqsave(&proc_lock)` → `&p->lock`.

- [ ] **Step 6: check the signal paths that still use `proc_lock` for `p->threads`**

  `grep -n 'proc_lock' kernel/ipc/signal.c kernel/sched/proc.c` — the
  remaining `proc_lock` uses in `signal_kill` / `signal_tkill`
  (`proc.c:~656,672`) walk `proc_list`, NOT `p->threads` directly for
  membership safety; `signal_tkill`'s inner `for (t = p->threads; ...)`
  is now UNDER-locked (it holds `proc_lock`, not `p->lock`). Leave it
  for Task 4 — add a `// FIXME(Task 4): walk p->threads under p->lock`
  comment at `proc.c` `signal_tkill`'s inner loop so it is not lost.

- [ ] **Step 7: build + gauntlet**

  `rm -f build/disk.img build/disk2.img && make test 2>&1 | tail -5`
  then the gauntlet. Both green. `smptest` in particular must still
  report `[smptest] ALL PASSED` every run.

- [ ] **Step 8: commit**

  ```bash
  git add kernel/
  git commit -m "SMP hardening: thread lists move from proc_lock to p->lock

  The per-process thread list (threads/zombies/live_threads) is now
  guarded by the per-process lock, not the global proc_lock, at all
  five mutation/scan sites. Same discipline as commit 04e4866, finer
  lock. signal_tkill's inner p->threads walk is left FIXME'd for the
  signal-send rework.

  <trailer>"
  ```

---

## Task 3: Thread reference count

**Files:**
- Modify: `kernel/sched/proc.h` (field + decls), `kernel/sched/thread.c`
  (impl + wiring), `kernel/sched/proc.c` (`proc_reap` drain,
  `process_exit`), `kernel/sched/sched.c` (`idle_entry` drain)

**Interfaces:**
- Produces:
  ```c
  // kernel/sched/proc.h
  void thread_get(struct thread *t);   // t->ref++
  void thread_put(struct thread *t);   // --t->ref; at 0: kfree(t->xstate); kfree(t)
  ```
  `struct thread` gains `uint32_t ref;`.

- [ ] **Step 1: field + impl**

  `kernel/sched/proc.h`: in `struct thread`, add near the top
  `uint32_t ref;  // references to this struct; freed at 0. NOT on_cpu.`
  Declare `thread_get` / `thread_put` next to `thread_kill`.

  `kernel/sched/thread.c`, near `thread_alloc`:
  ```c
  void thread_get(struct thread *t) {
      __atomic_add_fetch(&t->ref, 1, __ATOMIC_ACQ_REL);
  }

  void thread_put(struct thread *t) {
      if (__atomic_sub_fetch(&t->ref, 1, __ATOMIC_ACQ_REL) != 0) { return; }
      // The kernel stack and stack slot are released by whoever pulled
      // t off the zombie list, BEFORE this final put -- see thread_join
      // / proc_reap / idle_entry. Only the struct and xstate remain.
      kfree(t->xstate);
      kfree(t);
  }
  ```

- [ ] **Step 2: `thread_alloc` sets `ref = 1`**

  In `thread_alloc`, after the zeroing loop and before/near the tid
  line, set `t->ref = 1;` (the thread list's reference). Do this
  unconditionally (kernel threads get it too — the kzombies drain does
  the matching `thread_put`).

- [ ] **Step 3: replace the raw frees with `thread_put`**

  Three drain sites currently do `kfree(z->xstate); kfree(z);` (plus
  `pmm_free(z->kernel_stack_phys, ...)` and, in `thread_join`,
  `thread_stack_free`). Change each to keep the stack/slot free where it
  is and replace the two `kfree`s with a single `thread_put(z);`:
  - `kernel/sched/thread.c` `thread_join` (found-zombie branch)
  - `kernel/sched/proc.c` `proc_reap` (the `while (z)` loop)
  - `kernel/sched/sched.c` `idle_entry` (the `while (z)` kzombies drain)

  Order within each: `thread_wait_off_cpu(z);` →
  `pmm_free(z->kernel_stack_phys, KERNEL_STACK_ORDER);` →
  (`thread_join` only: `thread_stack_free(p, z->stack_slot);`) →
  `thread_put(z);`

- [ ] **Step 4: `thread_alloc` failure paths**

  In `thread_alloc`, the early-return paths that already `kfree(t)`
  before `t->ref` matters (`!t->xstate`) are fine as-is — `ref` was not
  yet published to any list. Leave them. In `thread_create`
  (`thread.c`), the `if (!t) { thread_stack_free(p, slot); return 0; }`
  path: `thread_alloc` returned NULL, nothing to put. The
  `thread_stack_free` on later `pmm_alloc` failure is unchanged. No
  `thread_put` needed on these — `thread_alloc` only ever returns a
  thread with `ref == 1` already owned by the list it was linked into.

- [ ] **Step 5: `process_exit` snapshot takes transient refs**

  In `kernel/sched/proc.c` `process_exit`, the sibling snapshot loop:
  under `p->lock`, for each sibling `t != self`, call `thread_get(t)`
  before storing it in `victims[nv++]`. After the loop and unlock:
  ```c
  for (int i = 0; i < nv; i++) {
      thread_kill(victims[i]);
      thread_put(victims[i]);
  }
  ```
  This closes the window where a snapshotted sibling could be freed by a
  concurrent same-process `thread_join` before `thread_kill` runs.

- [ ] **Step 6: add the concurrency selftest (this is the "failing test")**

  Create `kernel/sched/smp_lifetime_selftest.c` (add to `KERNEL_DIRS`
  build glob if needed — `kernel/sched` is already covered). It runs
  from a kernel thread after SMP is up:
  ```c
  // Spawns a helper process, has it create MANY threads that exit
  // immediately while another set joins them and a third tkills them,
  // then asserts the process's thread count returns to 1 (just main)
  // and no join returned -ESRCH for a tid that was still live.
  void smp_lifetime_selftest(void);
  ```
  Implement it against the real kernel API (`thread_create`,
  `thread_join`, `signal_tkill`, `current_proc()`), 4 rounds of 12
  threads. On success print `[smp] lifetime selftest passed`. On
  failure print `[smp] lifetime selftest FAILED: <what>`.

  Wire the call into the same place the other `smp_*_selftest` calls
  run (search: `grep -rn 'smp_steal_selftest' kernel/`). Add
  `"[smp] lifetime selftest passed"` to the `REQUIRED_MARKER` list in
  the `Makefile`.

- [ ] **Step 7: see it exercise the paths**

  `rm -f build/disk.img build/disk2.img && make test 2>&1 | grep -E 'lifetime|FAILED'`
  Expected: `[smp] lifetime selftest passed`. (It will pass now — Task 2
  already made the join path safe; this selftest is regression cover for
  the refcount and, in Task 4, the `_locked` senders. That is
  legitimate per the project's "guard test" precedent — see the SDD
  ledger ruling 5.)

- [ ] **Step 8: gauntlet, then commit**

  Gauntlet green. Then:
  ```bash
  git add kernel/ Makefile
  git commit -m "SMP hardening: reference count on struct thread

  thread_get/thread_put; the struct+xstate are freed at ref 0, the
  kernel stack still earlier by whoever drains the zombie list. Closes
  the process_exit snapshot UAF: siblings are ref'd across the SIGKILL.
  Adds smp_lifetime_selftest as ongoing regression cover.

  <trailer>"
  ```

---

## Task 4: `_locked` signal senders; `signal.c` walks under `p->lock`

**Files:**
- Modify: `kernel/ipc/signal.c`, `kernel/sched/proc.c` (`signal_tkill`,
  `signal_kill` single-pid path)

**Interfaces:**
- Consumes: `struct process` field `lock`; `thread_get/put`.
- Produces:
  ```c
  // kernel/ipc/signal.h  (static in signal.c is fine if no external caller)
  int signal_send_thread_locked(struct thread *t, int sig, struct siginfo *info);
  int signal_send_process_locked(struct process *p, int sig, struct siginfo *info);
  // Preconditions: caller holds p->lock (p == t->proc for the thread form).
  // signal_stop_cont_interlock + signal_is_ignored are the caller's job.
  ```

- [ ] **Step 1: split `signal_send_thread`**

  In `signal.c`, rename the body of `signal_send_thread` after the
  `signal_stop_cont_interlock` / `signal_is_ignored` guards into
  `signal_send_thread_locked(t, sig, info)`, which ASSUMES `p->lock`
  held: it does the `queue_append` / `sigset_add(&t->pending, sig)` and
  then `signal_wake_for_delivery(t)` — all under the held lock (drop the
  internal `spin_lock_irqsave(&p->lock)` / `spin_unlock` pair; the
  caller owns the lock). Return the same `int`.

  New thin `signal_send_thread`:
  ```c
  int signal_send_thread(struct thread *t, int sig, struct siginfo *info) {
      if (sig <= 0 || sig >= NSIG) { return -EINVAL; }
      struct process *p = t->proc;
      if (!p) { return -ESRCH; }
      signal_stop_cont_interlock(p, sig);
      if (signal_is_ignored(p, sig)) { return 0; }
      uint64_t f = spin_lock_irqsave(&p->lock);
      int rc = signal_send_thread_locked(t, sig, info);
      spin_unlock_irqrestore(&p->lock, f);
      return rc;
  }
  ```
  `signal_wake_for_delivery` under `p->lock` is deadlock-safe: see the
  spec §3 (a thread in `thread_exit_self` is `ZOMBIE`, so `thread_wake`
  early-returns without `wait_off_cpu`).

- [ ] **Step 2: split `signal_send_process`**

  Same shape. `signal_send_process_locked(p, sig, info)` ASSUMES
  `p->lock` held: `queue_append` / `sigset_add(&p->pending, sig)`, then
  the target-selection walk over `p->threads` (now safe — lock held),
  then `signal_wake_for_delivery(target)` under the lock. Thin
  `signal_send_process` acquires `p->lock` around the `_locked` call
  after the interlock/ignored guards.

- [ ] **Step 3: `signal_do_stop` / `signal_do_continue` walks under `p->lock`**

  `signal_do_stop`: the FIRST walk (`for (o = p->threads; ...)` marking
  siblings + `signal_wake_for_delivery`) currently runs with no lock —
  wrap it in `spin_lock_irqsave(&p->lock)` / `spin_unlock_irqrestore`.
  The later section that already takes `p->lock` for the `all_stopped`
  count and the `THREAD_STOPPED` publish is unchanged EXCEPT the
  `all_stopped` walk is already under the lock — leave it. Do NOT merge
  the two lock regions; the `schedule()` and the parent-wake happen
  between them and must stay outside.

  `signal_do_continue`: wrap the whole function body — the first walk
  (clearing `pending`/`stopping`) AND the second walk (`thread_wake(t,
  THREAD_STOPPED)`) — in a single `spin_lock_irqsave(&p->lock)` /
  `spin_unlock_irqrestore`. Remove the inner `spin_lock_irqsave` that
  only covered the first walk. `thread_wake` under `p->lock` is
  deadlock-safe (same argument). Keep the comment explaining why the
  second walk was "outside the lock" but update it: it is now inside,
  and safe because `thread_wake` only spins on a thread already
  committed to leaving its CPU.

- [ ] **Step 4: `signal_tkill` — remove the FIXME**

  In `kernel/sched/proc.c` `signal_tkill`: it currently holds
  `proc_lock`, walks `proc_list` to find the process by `tgid`, then
  walks `p->threads` for `tid`, then (with `proc_lock` released) calls
  `signal_send_thread(t, ...)`. Rework:
  ```c
  int signal_tkill(int tgid, int tid, int sig, struct siginfo *info) {
      if (sig < 0 || sig >= NSIG) { return -EINVAL; }
      struct process *p = (tgid > 0) ? proc_find(tgid) : /* see note */ 0;
      // NOTE: today tgid<=0 iterates every process; preserve that only
      // if a caller uses it. If sys_signal.c always passes tgid>0, drop
      // the loop. Check: grep -n 'signal_tkill' kernel/syscall/.
      if (!p) { return -ESRCH; }
      signal_stop_cont_interlock(p, sig);           // before p->lock
      int rc;
      uint64_t f = spin_lock_irqsave(&p->lock);
      struct thread *t = p->threads;
      while (t && t->tid != tid) { t = t->proc_next; }
      if (!t) { rc = -ESRCH; }
      else if (signal_is_ignored(p, sig)) { rc = 0; }
      else { rc = sig ? signal_send_thread_locked(t, sig, info) : 0; }
      spin_unlock_irqrestore(&p->lock, f);
      proc_put(p);        // proc_find returns ref'd as of Task 5; until
                          // then proc_find is borrow-only -- add the
                          // proc_put in Task 5, not here.
      return rc;
  }
  ```
  Because Task 5 has not landed yet, `proc_find` still returns a
  borrowed pointer here; DO NOT add `proc_put` in this task. Add a
  `// TODO(Task 5): proc_put(p)` marker.

- [ ] **Step 5: `signal_kill` single-pid path**

  `proc.c` `signal_kill`, the `if (pid > 0)` branch already does
  `proc_find` + `signal_send_process`. No `p->threads` walk here — it is
  fine. The group path (`for (p = proc_list; ...)`) stays on `proc_list`
  until Task 6. Leave it, but confirm it no longer needs `proc_lock`
  held across `sig_lock`: with the senders taking `p->lock` themselves,
  the only reason `proc_lock` is held across the loop is `proc_list`
  stability — note that in a comment for Task 6.

- [ ] **Step 6: build + gauntlet + selftest**

  `make test` — `[smp] lifetime selftest passed`, `[sigtest] ALL
  PASSED`, `[smptest] ALL PASSED`, no `[lock] PANIC`. Then gauntlet
  15/15.

- [ ] **Step 7: commit**

  ```bash
  git add kernel/
  git commit -m "SMP hardening: _locked signal senders; signal.c walks under p->lock

  signal_send_thread/process split into a thin wrapper that takes
  p->lock and an inner _locked that assumes it. signal_do_stop and
  signal_do_continue walk p->threads under p->lock. signal_tkill finds
  its target thread and delivers under one p->lock hold, so it no
  longer recurses into signal_send_thread. The wake path under p->lock
  is deadlock-safe: thread_wake early-returns for a ZOMBIE, which is
  what a thread in thread_exit_self is.

  <trailer>"
  ```

---

## Task 5: Process reference count; `proc_find` returns ref'd

**Files:**
- Modify: `kernel/sched/proc.h`, `kernel/sched/proc.c`,
  `kernel/sched/proc_table.c` / `.h`, `kernel/ipc/signal.c`,
  `kernel/syscall/sys_proc.c`

**Interfaces:**
- Produces:
  ```c
  void proc_get(struct process *p);   // p->ref++
  void proc_put(struct process *p);   // --p->ref; at 0: pid_free(pid); kfree(p)
  void proc_put_live(struct process *p);  // was proc_put: --live_threads;
                                          // at 0 frees address space + fds,
                                          // makes zombie, SIGCHLDs parent
  // proc_find / proc_table_lookup now return a pointer with a ref held,
  // or NULL. Every caller must proc_put it.
  ```
  `struct process` gains `uint32_t ref;`.

- [ ] **Step 1: rename `proc_put` → `proc_put_live`**

  In `proc.h` and `proc.c`, rename the existing `proc_put` (the
  `if (--p->refcount ...)` — now `--p->live_threads` after Task 1) to
  `proc_put_live`. Update its callers: `thread_exit_self`
  (`kernel/sched/thread.c`), and any `grep -rn 'proc_put' kernel/` hit
  that is the live-thread drop (there is essentially one: the
  `thread_exit_self` `p != NULL` branch). `proc_get` (the
  `p->refcount++` one used by `fork`) → rename to `proc_get_live` and
  make it `p->live_threads++`.

- [ ] **Step 2: `ref` field + `proc_get` / `proc_put`**

  `proc.h` `struct process`: add `uint32_t ref;` near `live_threads`
  with a comment distinguishing them.

  `proc.c`:
  ```c
  void proc_get(struct process *p) {
      __atomic_add_fetch(&p->ref, 1, __ATOMIC_ACQ_REL);
  }

  void proc_put(struct process *p) {
      if (__atomic_sub_fetch(&p->ref, 1, __ATOMIC_ACQ_REL) != 0) { return; }
      pid_free(&global_proc_table.pid_alloc, p->pid);
      kfree(p);
  }
  ```
  Include `sched/pid_alloc.h` / `sched/proc_table.h` as needed for
  `global_proc_table`.

- [ ] **Step 3: `proc_alloc` sets `ref = 1`; `proc_reap` drops it**

  `proc_alloc`: after zeroing, `p->ref = 1;` (the table's reference).
  Keep `proc_table_insert(p->pid, p)` — that publishes under the
  table's own ref.

  `proc_reap`: after `proc_table_remove(p)` and after the `proc_list`
  unlink (still present until Task 6), replace the implicit "freed via
  RCU" with an explicit `proc_put(p);` (drops the table ref grabbed at
  alloc). Remove the stale `// Note: p is freed via RCU callback`
  comment.

- [ ] **Step 4: `proc_table_lookup` returns ref'd**

  `proc_table.c` `proc_table_lookup`: wrap the bucket walk in the
  bucket lock (it currently uses `rcu_dereference` with no lock — Task 7
  swaps the macro, but take the lock NOW):
  ```c
  struct process *proc_table_lookup(int pid) {
      if (pid <= 0) { return 0; }
      struct proc_bucket *b = &global_proc_table.buckets[proc_hash(pid)];
      uint64_t f = spin_lock_irqsave(&b->lock);
      for (struct process *p = b->head; p; p = p->proc_next_hash) {
          if (p->pid == pid) {
              proc_get(p);
              spin_unlock_irqrestore(&b->lock, f);
              return p;
          }
      }
      spin_unlock_irqrestore(&b->lock, f);
      return 0;
  }
  ```
  `proc_find` (`proc.c`) just forwards, so it inherits the ref'd
  contract. Update its comment.

- [ ] **Step 5: fix every `proc_find` caller**

  Add a `proc_put` on every path out. The callers (from the spec's
  audit table):
  - `proc.c` `wait_for_pid`: `proc_put(p)` after `proc_reap(p)` returns,
    and on the `-EINTR` / not-found returns.
  - `proc.c` `proc_put_live` — the SIGCHLD-to-parent: `struct process
    *parent = proc_find(...)`; after the `signal_send_process` +
    `waitq_wake_all`, `proc_put(parent);` (guard the whole block on
    `if (parent)`).
  - `proc.c` `signal_kill` `pid > 0` branch: `proc_put(p)` after the
    send / on the zombie-state early return.
  - `signal.c` `signal_do_stop` parent wake (`proc_find(p->parent_pid)`):
    `proc_put(parent)` after `waitq_wake_all`.
  - `signal.c` `signal_tkill`: add the `proc_put(p)` the Task 4 TODO
    marked.
  - `sys_proc.c:115,122,135` (getpgid / setpgid / getsid style): each
    does `p = proc_find(a1)`; add `proc_put(p)` before every return in
    those handlers. `current_proc()` uses in the same functions are
    borrowed — no put.

  `grep -rn 'proc_find\|proc_table_lookup' kernel/` and check each hit
  has a matching `proc_put` on all paths.

- [ ] **Step 6: `fork` / `proc_alloc` interaction**

  `fork_task` builds a child via `proc_alloc` (ref 1 = table) and adds
  threads. The parent/child pid links are ints, not pointers — no ref.
  Confirm `fork_task` does not stash a `struct process *` past a lock
  drop; if it does, ref it.

- [ ] **Step 7: build + gauntlet**

  `make test`: watch for `[lock] PANIC` (a missed lock in
  `proc_table_lookup`) and for boot hangs (a missed `proc_put` will
  leak but not hang; a double `proc_put` will use-after-free and likely
  panic or triple-fault). All markers present. Gauntlet 15/15.

  Extra check for leaks: the existing leak-gate selftests (search
  `grep -rn 'leak' kernel/ userland/`) must still pass — a missing
  `proc_put` on the reap path would leak a process struct per spawn.

- [ ] **Step 8: commit**

  ```bash
  git add kernel/
  git commit -m "SMP hardening: reference count on struct process

  proc_get/proc_put (references); the old proc_put -> proc_put_live
  (live-thread count, address-space teardown). proc_table_lookup /
  proc_find take the bucket lock and return a ref'd pointer; every
  caller now proc_puts. proc_reap drops the table's reference
  explicitly instead of the inert RCU callback.

  <trailer>"
  ```

---

## Task 6: Streaming ref'd iterator; delete `proc_list`

**Files:**
- Modify: `kernel/sched/proc_table.c` / `.h`, `kernel/sched/proc.c`
  (`signal_kill` group path, `wait4`), `kernel/ipc/signal.c` (if the
  group walk lives there — it is `proc.c`), `kernel/sched/sched.h`,
  `userland/fork_test.c`

**Interfaces:**
- Produces:
  ```c
  // kernel/sched/proc_table.h
  // Calls fn(p, ctx) for every process present at some point during the
  // walk, with a reference held on p and NO lock held. fn must not
  // insert into or remove from the table, and must not call another
  // proc_table_for_each_ref (not reentrant). Order unspecified.
  void proc_table_for_each_ref(void (*fn)(struct process *p, void *ctx),
                               void *ctx);
  ```
  `struct process` gains `uint32_t iter_gen;`. `proc_list` and
  `struct process *next` are DELETED.

- [ ] **Step 1: generation stamp + iterator**

  `proc_table.h`: `struct process` forward stays; add to
  `struct proc_table` a `uint32_t iter_gen; struct spinlock iter_lock;`
  (init in `proc_table_init`, `iter_lock` at `LOCK_RANK_PROCTABLE`).
  Add `uint32_t iter_gen;` to `struct process` in `proc.h`.

  `proc_table.c`:
  ```c
  void proc_table_for_each_ref(void (*fn)(struct process *, void *),
                               void *ctx) {
      uint64_t gf = spin_lock_irqsave(&global_proc_table.iter_lock);
      uint32_t gen = ++global_proc_table.iter_gen;
      spin_unlock_irqrestore(&global_proc_table.iter_lock, gf);

      for (int i = 0; i < PROC_HASH_BUCKETS; i++) {
          struct proc_bucket *b = &global_proc_table.buckets[i];
          for (;;) {
              struct process *batch[8];
              int n = 0;
              uint64_t f = spin_lock_irqsave(&b->lock);
              for (struct process *p = b->head; p && n < 8;
                   p = p->proc_next_hash) {
                  if (p->iter_gen == gen) { continue; }
                  p->iter_gen = gen;
                  proc_get(p);
                  batch[n++] = p;
              }
              spin_unlock_irqrestore(&b->lock, f);
              for (int k = 0; k < n; k++) {
                  fn(batch[k], ctx);
                  proc_put(batch[k]);
              }
              if (n < 8) { break; }   // bucket fully stamped
          }
      }
  }
  ```
  `iter_gen` on `struct process` starts 0 (from the `proc_alloc`
  zeroing); the counter starts at 1 after the first `++`, so a
  never-iterated process never spuriously matches.

- [ ] **Step 2: convert `signal_kill` group path**

  `proc.c` `signal_kill`, the `pid <= 0` branch: replace the
  `spin_lock_irqsave(&proc_lock)` + `for (p = proc_list; ...)` with:
  ```c
  struct kill_ctx { int pid; int target_pgid; int sig;
                    struct siginfo *info; int found; int rc; };
  static void kill_one(struct process *p, void *v) {
      struct kill_ctx *c = v;
      if (p->state == PROC_ZOMBIE) { return; }
      if (c->pid != -1 && p->pgid != c->target_pgid) { return; }
      c->found = 1;
      if (c->sig) {
          int r = signal_send_process(p, c->sig, c->info);
          if (r != 0) { c->rc = r; }
      }
  }
  ```
  then in `signal_kill`: build the ctx, `proc_table_for_each_ref(kill_one,
  &ctx)`, `return ctx.found ? ctx.rc : -ESRCH;`. `signal_send_process`
  takes `p->lock` itself; no lock is held across it now.

  `signal_tkill`: already reworked in Task 4 to `proc_find` a single
  `tgid`. If it still has a `tgid <= 0` iterate-all path, convert that
  with the same iterator pattern; otherwise it is done.

- [ ] **Step 3: convert `wait4`**

  `proc.c` `wait4`: the `for (p = proc_list; ...)` child scan → iterator.
  The wrinkle: `wait4` returns from inside the loop on a zombie child
  (`proc_reap` + return the pid). The callback cannot return up. Use the
  ctx to record the first matching zombie, then act after the walk:
  ```c
  struct wait_ctx { struct process *self; int pid; int found;
                    struct process *zombie; int stopped_pid; };
  static void wait_scan(struct process *p, void *v) {
      struct wait_ctx *c = v;
      if (p->parent_pid != c->self->pid) { return; }
      if (c->pid > 0  && p->pid  != c->pid)        { return; }
      if (c->pid == 0 && p->pgid != c->self->pgid) { return; }
      if (c->pid < -1 && p->pgid != -c->pid)       { return; }
      c->found = 1;
      if (p->state == PROC_ZOMBIE && !c->zombie) {
          proc_get(p);            // keep it past the walk's own put
          c->zombie = p;
      }
      // WUNTRACED handled after the walk via stopped_pid, same idea
  }
  ```
  After the walk: if `c.zombie`, `encode_status` + `proc_reap` +
  `proc_put(c.zombie)` + return its pid. Else `WUNTRACED` check. Else
  `-ECHILD` if `!found`, `0` if `WNOHANG`, else
  `waitq_sleep(&self->child_waiters, 0)` and loop.

  Be careful: `proc_reap` frees the process; call `proc_put(c.zombie)`
  AFTER `proc_reap` has done `proc_table_remove` + its own `proc_put`
  (the ref we took in `wait_scan` is what keeps the struct alive across
  `encode_status`). Actually `proc_reap` ends in `proc_put`; our extra
  ref means the struct survives until our `proc_put`. Order:
  `int st = encode_status(c.zombie); proc_reap(c.zombie); if (status)
  *status = st; int r = c.zombie->pid; proc_put(c.zombie); return r;`
  — read `pid` before the final put. Store `pid` in a local BEFORE
  `proc_put`.

- [ ] **Step 4: delete `proc_list`**

  - `proc.c`: delete `struct process *proc_list;`, the
    `spin_lock_irqsave(&proc_lock)` + insert in `proc_alloc`, the
    `spin_lock_irqsave(&proc_lock)` + unlink in `proc_reap`, and the
    `spin_init(&proc_lock, ...)` in `process_init` and the
    `struct spinlock proc_lock;` definition.
  - `sched.h`: delete `extern struct process *proc_list;` and
    `extern struct spinlock proc_lock;`.
  - `proc.h`: delete `struct process *next;` from `struct process`.
  - `grep -rn 'proc_list\|proc_lock' kernel/` must return nothing.

- [ ] **Step 5: `fork_test.c` PID-visibility assertion**

  In `userland/fork_test.c`, add (and call from `main`, incrementing the
  file's `failures` counter on mismatch):
  ```c
  static void check_pid_visibility(void) {
      int pid = fork();
      if (pid == 0) { _exit(7); }
      if (kill(pid, 0) != 0) {
          printf("[forktest] FAILED: child not visible by pid\n"); failures++;
      }
      int st = 0;
      waitpid(pid, &st, 0);
      if (kill(pid, 0) == 0) {
          printf("[forktest] FAILED: reaped child still visible\n"); failures++;
      }
      if (!failures) { printf("[forktest] pid visibility passed\n"); }
  }
  ```
  (If `fork_test.c`'s success line is not already a `REQUIRED_MARKER`,
  wire `[forktest] pid visibility passed` in, or fold the assertion into
  `vfstest.c` which is a required marker.)

- [ ] **Step 6: build + gauntlet ×1**

  `make test`: `[forktest] pid visibility passed`, all markers, no
  panic. Gauntlet 15/15.

- [ ] **Step 7: commit**

  ```bash
  git add kernel/ userland/fork_test.c
  git commit -m "SMP hardening: streaming ref'd proc_table iterator; drop proc_list

  proc_table_for_each_ref walks the table in batches of 8, handing the
  callback a ref'd process with no lock held, resuming per bucket by a
  generation stamp. signal_kill's group path and wait4's child scan use
  it. proc_list, proc_lock and struct process::next are deleted -- the
  hash table is the only process store.

  <trailer>"
  ```

---

## Task 7: Delete RCU

**Files:**
- Delete: `kernel/sync/rcu.c`, `kernel/sync/rcu.h`
- Modify: `kernel/sched/proc.h`, `kernel/sched/proc_table.c` / `.h`,
  `kernel/sched/thread_table.c` / `.h`, `kernel/sched/proc.c`,
  `Makefile`, any `#include "sync/rcu.h"` site

- [ ] **Step 1: remove `call_rcu` from the tables**

  `proc_table.c`: `proc_table_remove` currently ends `spin_unlock...;
  call_rcu(&proc->rcu, proc_rcu_free);`. Replace with just the unlock
  followed by `proc_put(proc);` (drop the table's reference — Task 5
  already added the matching `proc_get` at `ref = 1`... actually `ref`
  starts at 1 for the table; `proc_table_remove` is that drop). Delete
  `proc_rcu_free` and the `#include "sync/rcu.h"`.

  Wait — cross-check with Task 5 Step 3: `proc_reap` calls
  `proc_table_remove(p)` then `proc_put(p)`. If `proc_table_remove` also
  `proc_put`s, that is two drops for one table ref. RESOLVE: the table
  ref is dropped exactly once, in `proc_table_remove`. Remove the
  explicit `proc_put(p)` that Task 5 Step 3 added to `proc_reap` — leave
  a comment there: `// the table's ref is dropped by proc_table_remove`.
  (Task 5 stands alone because until this task `proc_table_remove` still
  `call_rcu`s a no-op, so `proc_reap` needed its own put. This task
  moves the put into `proc_table_remove` and removes the duplicate.)

  `thread_table.c`: `thread_table_remove` ends
  `call_rcu(&t->rcu, thread_rcu_free);`. The thread struct's lifetime is
  the thread-list reference (Task 3), NOT the thread_table entry —
  `thread_table_remove` just unlinks the bucket. Delete the `call_rcu`
  and `thread_rcu_free` entirely. Delete `#include "sync/rcu.h"`.

- [ ] **Step 2: swap `rcu_dereference` / `rcu_assign_pointer`**

  In `proc_table.c` and `thread_table.c`, replace `rcu_dereference(x)`
  with `__atomic_load_n(&(x), __ATOMIC_ACQUIRE)` and
  `rcu_assign_pointer(a, b)` with
  `__atomic_store_n(&(a), (b), __ATOMIC_RELEASE)`. The lookups are
  bucket-locked now (Task 5), so these are just publication barriers.

- [ ] **Step 3: `thread_table_lookup` returns ref'd**

  For consistency with `proc_table_lookup`: take the bucket lock,
  `thread_get(t)` before returning. Audit callers (`grep -rn
  'thread_table_lookup' kernel/`) — there are ~zero real ones today
  (join uses the list), so add `thread_put` to whatever exists, or if
  genuinely unused, leave the function ref'd and note it.

- [ ] **Step 4: strip `rcu_head` and the files**

  - `proc.h`: delete `struct rcu_head rcu;` from `struct process` and
    `struct thread`; delete `#include "sync/rcu.h"`.
  - `rm kernel/sync/rcu.c kernel/sync/rcu.h`.
  - `Makefile`: remove `rcu.o` / `sync/rcu.o` from the objects list and
    any explicit rule.
  - `grep -rn 'rcu\|call_rcu\|synchronize_rcu\|rcu_head' kernel/` — only
    the `__atomic` swaps' surrounding comments should remain; no symbol
    references.

- [ ] **Step 5: build + gauntlet ×3**

  `make test` clean. Then run the gauntlet **three times** back to
  back (`for i in 1 2 3; do bash .../gauntlet.sh || break; done`) —
  this is the milestone's final stability gate. 45/45.

- [ ] **Step 6: commit**

  ```bash
  git add -A kernel/ Makefile
  git commit -m "SMP hardening: delete the inert RCU

  rcu.c/.h are removed. call_rcu is replaced by the reference counts:
  proc_table_remove drops the table's process ref, thread_table_remove
  just unlinks the bucket (the thread-list ref owns the struct).
  rcu_dereference/assign become plain acquire/release atomics under the
  now-bucket-locked lookups. rcu_head is gone from both structs.

  <trailer>"
  ```

---

## Task 8: Documentation

**Files:**
- Modify: `docs/optimization-summary.md`,
  `docs/abi-compatibility.md`,
  `docs/superpowers/specs/2026-08-31-smp-hardening-handoff.md`,
  `.superpowers/sdd/2026-08-31-phase14-input-and-solidity/progress.md`

- [ ] **Step 1: `optimization-summary.md`**

  In the "Phase 13.5" section, move the "Found, NOT fixed" and "Also
  latent" bullets that this milestone resolved into a new subsection
  "Phase 13.6: lifetime + lock detangle (commits `<first>..<last>`)"
  summarising: refcounts on thread/process, `p->lock` as the sole
  thread-list guard, `_locked` senders, streaming iterator, `proc_list`
  + RCU deleted, processes no longer leak. Update the "Next Steps"
  carried-forward list: strike the three items now done; the remaining
  real item is the per-process-lock coarseness (a Phase 15 concern) and
  the block-cache write-through.

- [ ] **Step 2: `abi-compatibility.md`**

  Add a line under whatever "internal / no ABI impact" heading fits:
  the lifetime + locking rework changed no struct that crosses to
  userland and no syscall semantics — `thread_join`, `kill`, `tgkill`,
  `wait4` behave identically, only correctly under SMP contention.
  If the file has a "last refreshed" date, bump it.

- [ ] **Step 3: handoff + ledger close-out**

  In `2026-08-31-smp-hardening-handoff.md`'s header UPDATE block, mark
  the signal.c walks and Task 6 as **done** with the commit range, and
  note RCU is removed. In the SDD `progress.md`, append a "session 3"
  entry: milestone complete, gauntlet 45/45, Phase 14 plan can resume
  at Task 2 with no SMP caveat.

- [ ] **Step 4: commit**

  ```bash
  git add docs/ .superpowers/
  git commit -m "SMP hardening: close out the lifetime + lock-detangle milestone

  Phase 13.6 in optimization-summary.md; abi-compatibility notes no ABI
  impact; handoff and SDD ledger marked complete. gauntlet 45/45.

  <trailer>"
  ```

---

## Self-Review

**Spec coverage:**
- §1 thread refcount → Task 3. §1 process refcount → Task 5. §1
  `proc_put_live` rename → Task 5 Step 1. ✓
- §2 `p->lock` rename → Task 1; thread-list sites → Task 2; `kzombies_lock`
  → Task 1 Step 3. ✓
- §3 `_locked` senders → Task 4; `signal_tkill` → Task 4 Step 4;
  `signal_kill` group → Task 6 Step 2; `signal_do_stop`/`continue`
  walks → Task 4 Step 3. ✓
- §4 bucket-locked ref'd lookup → Task 5 Step 4; `proc_table_for_each_ref`
  + generation stamp → Task 6 Step 1; consumers → Task 6 Steps 2-3;
  `proc_list` delete → Task 6 Step 4. ✓
- §5 RCU removal → Task 7 (all four sub-points). ✓
- Spec "Implementation slices" 1-8 map 1:1 to Tasks 1-8. ✓
- Spec testing (fork_test assertion, concurrency selftest, gauntlet) →
  Task 3 Step 6, Task 6 Step 5, Global Constraints. ✓
- Spec `proc_find` caller audit table → Task 5 Step 5 (same list). ✓

**Placeholder scan:** The `signal_tkill` `tgid <= 0` branch is marked
"check whether any caller uses it" rather than resolved — this is a
genuine "read the caller and decide" step, not a hidden TODO; the two
outcomes (keep the iterate-all path / drop it) are both spelled out.
Task 7 Step 1 explicitly resolves the Task 5 / Task 7 double-`proc_put`
overlap. No "add error handling" / "similar to Task N" / bare "write
tests" placeholders.

**Type consistency:** `thread_get`/`thread_put`/`proc_get`/`proc_put`/
`proc_put_live`/`proc_get_live` used consistently. `proc_table_for_each_ref`
signature identical in the interface block and the impl. `iter_gen` /
`iter_lock` named the same in Task 6 Steps 1 and the impl. `p->lock` (not
`p->plock` or `proc->lock`) throughout after Task 1. `live_threads` (not
`live_thread_count`) throughout.

**Ordering hazard checked:** Task 5 adds `proc_put(p)` in `proc_reap`;
Task 7 moves that drop into `proc_table_remove` and removes the
duplicate — called out explicitly in Task 7 Step 1 so an executor doing
the tasks in order does not leave a double-free.
