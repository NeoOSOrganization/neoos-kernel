# Work Stealing: Problem Brief

**Status: RESOLVED — see `2026-08-30-work-stealing-resolution.md`.**
This document is kept as the record of the investigation. Its §6
suggested approach was followed with one change: §4.1's `prev_pending`
is not sufficient on its own and, alone, introduces the very regression
§2 could not isolate. The resolution explains why.

**Original status:** blocked, not landed. Written for a fresh session —
assume no prior context.
**Date:** 2026-08-30, at the close of Phase 10.
**Baseline:** commit `af0345d`. Stable: `make test` green, 6/6 userland
suites, 4 CPUs online, three consecutive clean runs.

---

## 1. What works today

NeoOS boots on 4 CPUs. Kernel threads run genuinely in parallel —
`smp_parallel_selftest` puts 8 threads × 10,000 locked increments across
all four CPUs and asserts the exact total 80,000, so the spinlocks are
provably mutually exclusive across CPUs. TLB shootdown, the reschedule
IPI and the panic-stop NMI all work.

Kernel threads reach other CPUs only through **explicit placement**
(`thread_alloc_kernel_on(entry, cpu_index)`). Nothing migrates on its
own.

## 2. What is blocked

`steal_work()` — an idle CPU taking a thread from the busiest remote
queue. The mechanism is written and its selftest passes; the problem is
what migration *exposes elsewhere*.

**Symptom:** with stealing enabled the kernel becomes nondeterministic —
2 to 4 of the (then) 5 userland suites pass, varying run to run, with
page faults inside `kmalloc` at `cr2=0x100000000`. The same tree with
stealing disabled is stable.

**Crucially: I could not get back to green by disabling stealing alone.**
A branch carrying the stealing work with `steal_work` returning 0 was
still flaky where `af0345d` is stable. So at least one *other* change on
that branch is harmful and was never isolated. Do not assume the
stealing code is the only suspect.

## 3. Where the work is

`git stash@{0}` on `main` — 13 files, ~291 insertions. Contains
`steal_work`, the `prev_pending` mechanism, deferred zombie publication,
`thread_alloc_kernel_on`/`_unqueued`, the waitq double-free fix, the
per-CPU syscall MSR fix, and the early-AP-bringup reorder, all mixed
together. **Its value is as a record of the investigation, not as a
patch to apply.** Landing it wholesale reproduces the instability.

## 4. Bugs already found and understood

Five real defects, all latent on one CPU. Numbers 3–5 exist **at
`af0345d` right now** and are worth fixing regardless of stealing.

### 4.1 `schedule()` published `prev` before saving its context — FIXED in stash

```c
prev->state = THREAD_READY;
enqueue_ready(prev);                      // visible to stealers HERE
...
context_switch(&prev->saved_rsp, ...);    // saved_rsp written HERE
```

Between those lines a stealer can dequeue `prev` and switch into it
using a **stale `saved_rsp`** — two CPUs executing on one kernel stack.

Fix in the stash: a per-CPU `prev_pending`, flushed by whoever runs next
on that CPU (at the top of `schedule()` *and* after `context_switch`,
because a freshly created thread starts at its trampoline and never
reaches the post-switch path). **Note the subtlety that bit once:**
`prev_pending` must be set only *after* the `prev == next` early return,
or a still-RUNNING thread gets requeued.

### 4.2 `idle_init_for` allocated the idle thread onto a run queue — FIXED in stash

`thread_alloc_kernel` enqueues; `idle_init_for` then called
`dequeue_specific`. A stealer can take the idle thread in between, after
which `cpus[i].idle` names a thread running on another CPU. Fixed with
`thread_alloc_kernel_unqueued`. The same alloc-then-move shape appeared
in the SMP selftests and needed `thread_alloc_kernel_on`.

### 4.3 `waitq.c` double-frees its selftest sleeper — **LIVE at HEAD**

```c
while (selftest_stage != 2) { schedule(); }
pmm_free(t->kernel_stack_phys, KERNEL_STACK_ORDER);
kfree(t);
```

The comment claims the sleeper "is a ZOMBIE now and not running", but
`selftest_stage = 2` is set *before* `thread_exit_self` — the sleeper is
still executing its own exit path on that stack. `idle_entry`'s
`kzombies` drain frees it again. One heap slot lands on the free list
twice, so a later `kmalloc` hands the same memory to two owners.

This was diagnosed from a corrupted `free_list` pointer of `0xfffffffc`
— which is `-4`, the idle tid for CPU 3. **Fix:** delete the manual
free; kernel threads are owned by `idle_entry`'s drain.

### 4.4 `thread_exit_self` publishes itself as reapable while still running — **LIVE at HEAD**

It pushes to `kzombies` (or `p->zombies` + wakes `join_waiters`) and
*then* calls `schedule()`. Four sites `pmm_free` a thread's kernel stack
(`sched.c`, `thread.c`, `proc.c`, `waitq.c`); until `context_switch`
completes the thread is still on that stack. Safe on one CPU by
construction; a use-after-free on four.

Fix in the stash: move the publish into `thread_finish_exit(t)`, called
from the `prev_pending` flush — i.e. it depends on 4.1.

### 4.5 `syscall_init` programs per-CPU MSRs on the BSP only — **LIVE at HEAD**

`EFER.SCE`, `STAR`, `LSTAR`, `SFMASK` are per-CPU. A user thread that
reaches an AP would `SYSCALL` into an unconfigured `LSTAR`. Reachable
today without stealing, because `enqueue_ready` uses `this_cpu()` — a
user thread woken by a kernel thread running on an AP lands there.

Observed as **every userland suite producing no output at all, with no
exception logged**. Fix: split out `syscall_init_this_cpu()` and call it
from `ap_main`. Small, self-contained, worth landing on its own.

## 5. The unsolved part

With 4.1–4.5 fixed and stealing on, the kernel still failed. Two further
findings, neither resolved:

### 5.1 `signal_do_continue` has a lost wakeup

```c
/* stop path, signal.c ~320 */      /* continue path */
t->state = THREAD_STOPPED;          if (t->state == THREAD_STOPPED) {
schedule();                             t->state = THREAD_READY;
                                        thread_enqueue_ready(t); }
```

SIGCONT landing after the thread decides to stop but before it sets the
state is lost, and the thread stops forever. Observed as `sigtest`
hanging after `SIGSEGV on sigaltstack`. The reverse order is equally
broken: SIGCONT can enqueue the thread while it is still running toward
`schedule()`, so it is both requeued and picked up elsewhere.

A raw state flip cannot be made safe here. This wants the
release-lock-then-block handoff `waitq_sleep` already implements — but
`waitq_sleep` sets `THREAD_BLOCKED`, and `WIFSTOPPED` reporting depends
on `THREAD_STOPPED` being distinguishable, so it is a redesign of the
stop path rather than a substitution.

### 5.2 Pinning userland made things *worse*, and I do not know why

Hypothesis: user threads migrate through **wakeups**, not just stealing
(`enqueue_ready` uses `this_cpu()`), so restricting `steal_work` alone
does not pin userland. Forcing `t->proc` threads onto CPU 0 in
`enqueue_ready` *should* have restored the pre-SMP behaviour.

It did the opposite: 2/5 suites with pinning versus 4/5 without. That is
backwards and unexplained. **It is the most informative loose thread in
this brief** — whatever makes pinning harmful is probably the same thing
making stealing harmful.

## 6. Suggested approach

1. **Land 4.3, 4.4 and 4.5 as three separate commits on `af0345d`**,
   verifying `make test` green after each. They are real bugs
   independent of stealing, and they shrink the search space.
2. **Then 4.1 (`prev_pending`) alone**, and re-verify. If the tree goes
   flaky here, that isolates the harmful change hinted at in §2.
3. **Then 5.1**, the SIGSTOP/SIGCONT handshake.
4. **Only then re-enable `steal_work`.**

Do not batch these. The reason this is blocked is that eight changes
went in together and the regression could not be bisected afterwards.

## 7. Tools

- `make test` — headless, COM1 to `build/serial.log`, fails on any
  `FAILED` line, a missing boot marker, or a missing suite marker
  (`REQUIRED_MARKERS`). Rebuilds the disk images each run.
- `make test SMP_CPUS=1` — the fastest way to tell an SMP bug from a
  general one. Every SMP selftest fails here by design.
- The **lock rank checker** is the most valuable instrument in the
  kernel: it turns an ordering error into a panic naming both locks
  instead of a hang. It caught two real inversions during Phase 10.
- `x86_64-elf-addr2line -f -e build/kernel.elf <rip>` on the `rip=` in
  an exception dump. Then `objdump -d --start-address=...` — reading the
  faulting instruction is what identified the corrupted free-list
  pointer; register archaeology alone went in circles.

## 8. Reproducing

```bash
git stash list                     # stash@{0} is the investigation
git stash show -p stash@{0} | less # do not apply wholesale -- see §3
make test                          # baseline: green at af0345d
```
