# Work Stealing: Resolution

**Status:** landed. Supersedes
`2026-08-30-work-stealing-problem.md`, which is kept as the record of
the investigation that produced it.
**Baseline:** `af0345d` (Phase 10 close).

---

## 1. What the brief got right, and the one thing it missed

The brief listed five defects (§4) and two unresolved findings (§5). All
seven were real. But it framed the problem as *"work stealing exposes
latent bugs"*, and the shape of the fix follows from a sharper framing:

> **A thread must not be reachable by any other CPU while a CPU is still
> executing on its kernel stack.**

Every one of the five defects is an instance of that single rule being
broken, and — crucially — so is the unexplained flakiness of §2, which
the brief could not isolate.

The brief's own proposed fix for §4.1, a per-CPU `prev_pending` slot,
handles exactly one of the ways a thread becomes reachable too early:
`schedule()` requeueing the thread it is switching away from. It does
nothing about the others. Worse, on its own it **introduces** a new
defect:

```
CPU A: thread T calls waitq_sleep -> state = BLOCKED -> schedule()
       -> c->prev_pending = T -> context_switch (in progress)
CPU B: waitq_wake_one(q) -> T->state = READY -> enqueue on B's queue
CPU A: next thread flushes prev_pending, sees state == READY,
       enqueues T *again* on A's queue
```

T is now on two run queues. Two CPUs run it, on one kernel stack. This
is reachable with `steal_work()` returning 0, which is precisely the
symptom §2 describes as "at least one *other* change on that branch is
harmful and was never isolated".

## 2. The mechanism that replaces it

Two pieces, which only work together.

### `prev_pending` — the hand-off

`schedule()` no longer requeues the outgoing thread. It parks it in
`cpus[].prev_pending`, and `sched_post_switch()` — running as the
*incoming* thread, the earliest point at which the outgoing thread's
context is definitely saved — releases it.

`sched_post_switch()` is called from three kinds of place, which
together cover every way a CPU can arrive on a new thread:

1. the top of `schedule()`, for a thread resumed by an earlier switch;
2. immediately after `context_switch()` returns, for the common case;
3. **the head of each of the three trampolines.** A brand-new thread
   starts at its trampoline and never reaches (2). Without this the
   outgoing thread sits in `prev_pending`, runnable nowhere, until that
   CPU next schedules — which for a new thread that goes straight to
   userland and stays there is not bounded at all.

Linux calls (2) `finish_task_switch` and (3) `schedule_tail`; the split
is the same one, for the same reason.

### `thread->on_cpu` — the barrier

Set when a CPU switches *to* a thread, cleared by `sched_post_switch()`
once the switch *away* has completed. It means "some CPU is executing on
this thread's kernel stack".

- **Wakers wait for it.** `thread_wake(t, from)` is now the single door
  every wake goes through — waitq wake, sleep timeout, signal delivery,
  SIGCONT. It spins until `on_cpu` clears, then does the
  `BLOCKED`/`STOPPED` → `READY` move as a **compare-exchange**, so that
  among several racing wakers exactly one enqueues.
- **Reapers wait for it.** The three sites that free a thread's kernel
  stack — `idle_entry`'s `kzombies` drain, `proc_reap`, `thread_join` —
  call `thread_wait_off_cpu()` first.

The two pieces compose rather than collide: since no waker may touch
`state` while `on_cpu` is set, `sched_post_switch()` always reads a
state nobody has raced it for. The double-enqueue above cannot occur.

### Two assertions

A violation now names itself instead of surfacing as heap corruption ten
milliseconds later:

- `schedule()` panics if it is about to run a thread whose `on_cpu` is
  still set;
- `wait_off_cpu()` panics rather than spinning forever on a thread that
  is never going to leave its CPU.

## 3. Disposition of each item in the brief

| Brief | Verdict |
|---|---|
| §4.1 `schedule()` publishes `prev` early | Fixed, by `prev_pending` **plus** `on_cpu`. The brief's fix alone was insufficient and introduced §2's regression. |
| §4.2 `idle_init_for` queues then dequeues | Fixed. `thread_alloc_kernel_unqueued`/`_on` place threads correctly up front; `dequeue_specific` deleted. |
| §4.3 `waitq.c` double-free | Fixed. The manual free is gone; `idle_entry`'s drain owns kernel threads. |
| §4.4 `thread_exit_self` publishes while running | Fixed on the **reaper's** side, not by deferring publication. See §4 below. |
| §4.5 syscall MSRs on the BSP only | Fixed, with a selftest that reads LSTAR back on every CPU. |
| §2 unisolated harmful change | **Identified**: the `prev_pending` double-enqueue described above. |
| §5.1 SIGCONT lost wakeup | Fixed with a `stopping` flag published under `sig_lock`. |
| §5.2 pinning made things worse | **Not reproduced, and now moot.** See §5. |

## 4. Where §4.4's fix differs from the brief

The brief proposed moving the exit publication into
`thread_finish_exit()`, called from the `prev_pending` flush. That was
rejected. `thread_exit_self` publishes *and* calls `proc_put`, which
frees the address space, releases the fd table and sends `SIGCHLD`;
running all of that from `sched_post_switch()` means running it under
the **incoming** thread's CR3, and `proc_put`'s "leave the dying address
space before freeing it" `mov cr3` would clobber it.

The danger was never the publication — it was the free. So the wait
moved to the three reapers, which is smaller, needs no re-entrancy
analysis, and reuses the `on_cpu` handshake already required for wakes.

## 5. Two things the brief did not know

### The APs were never preempted

The scheduler clock has been the **LAPIC timer** since it replaced the
PIT, and the LAPIC timer is a per-CPU device — but only the BSP ever
programmed one. Every AP ran with no time slicing at all: a
compute-bound thread that reached one held that core until it blocked or
exited.

That is not a foundation work stealing can be built on, and it is very
likely part of why §5.2's pinning experiment produced results that
"[made] no sense". Several comments in the tree blamed the IOAPIC for
this; the IOAPIC routes the keyboard, and the timer was never its
business.

`ap_main` now calls `timer_init_this_cpu()`. The wall clock stays
BSP-only (`tick_count++` from four CPUs would both lose updates and run
the clock at 4× speed); the time slice moved into `struct cpu`.

### `tlb_shootdown()` leaked interrupt state

It does `sti` before waiting for acknowledgements — correct, and
required by its own deadlock analysis — and never restored the caller's
IF. `kmain`, whose only shootdown is the selftest, therefore continued
booting with interrupts enabled and was scheduled away mid-boot, never
printing the "starting scheduler" marker. Latent at `af0345d`; exposed
the moment AP bringup moved before the spawns.

## 6. Decisions taken

**Stealing is not restricted to kernel threads.** The first attempt took
kernel threads only, on the theory that a user thread's address space,
fd table and signal state carried single-CPU assumptions. They did — but
the assumptions were in the *scheduler*, not in the process code. The
per-CPU state a user thread actually needs (CR3, `TSS.rsp0`, the xstate
area, GS) is already reloaded on every switch, and the filesystem stack
(ATA, blkcache, VFS, the `fs_lock` mutex) is properly locked. The four
real hazards were the SYSCALL MSRs, early publication, reaper
use-after-free, and the SIGCONT race — all now fixed and asserted.

**No `smp_set_stealing()` switch.** The brief's branch carried one
because stealing was broken. A knob nobody turns is a knob that rots;
if stealing regresses, that is something to fix, not to hide.

**Idle CPUs are not poked on enqueue.** An idle CPU discovers stealable
work on its next local timer tick — at most 10ms — instead of every
`enqueue_ready()` sending an IPI. Latency for cost, revisit if it
measures.

**AP bringup moved before the spawns.** APs now come online into an
empty system. Bringing them up afterwards meant each one immediately
started running user processes while the BSP was still bringing up the
next AP and running selftests — and `tlb_shootdown_selftest` asserts an
exact free-frame count across a shootdown, which is only meaningful
while nothing else is allocating.

## 7. Evidence

`make test` (4 CPUs, headless QEMU, 60s):

```
[syscall] per-cpu msr selftest passed, cpus=4
[tlb] shootdown selftest passed, acks=3
[smp] local timer selftest passed, cpus=4
[smp] parallel selftest passed, cpus=4
[smp] steal selftest passed, cpus=2
[smptest] cpus=4, running on cpu 2
[smptest] user threads ran on 3 cpus
```

Note `running on cpu 2`: `smptest`'s own main thread was stolen off the
BSP before it printed. Three new selftests back the three new
invariants, and each was verified by reverting its fix and watching it
fail (the per-CPU MSR one reports `cpu=1 lstar=0`).

`sigtest` gained a SIGSTOP/SIGCONT race loop.

**It is a smoke test, not a proven reproducer, and §5.1's fix rests on
construction rather than on it.** Reverting the fix was tried twice —
before migration, when a parent's `SIGSTOP` and `SIGCONT` are serialised
and the child never runs between them, and after, when it does — and the
suite passed both times. The vulnerable window is the few microseconds
between a thread taking the stop signal out of its pending set and
publishing `THREAD_STOPPED`, and ten rounds do not land in it. Making
this deterministic needs a deliberate widening of that window behind a
debug switch; that is worth doing, and is not done here.

## 8. Known gaps

- **No load balancing, only stealing.** A CPU takes work when its own
  queue is empty; nothing rebalances two unequally loaded busy CPUs.
  Scheduling classes (roadmap milestone 8) is where that belongs.
- **Victim choice is a linear scan** of `ready_count` over all online
  CPUs on every idle `schedule()`. Fine at 4 CPUs, not at 128.
- **No affinity.** There is no `sched_setaffinity`, so `sched_getcpu()`
  is a sample rather than a fact, and a stolen thread loses its cache
  locality with nothing to weigh that against.
- **`FS_BASE` is not part of the context switch.** It does not need to
  be yet — NeoOS has no TLS — but migration makes it mandatory the
  moment `arch_prctl(ARCH_SET_FS)` exists. Recorded here so the TLS
  milestone does not have to rediscover it.
