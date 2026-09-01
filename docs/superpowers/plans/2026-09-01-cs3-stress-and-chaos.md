# CS3 — Stress matrix and chaos harness — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the interleavings that produce SMP bugs happen on
purpose, and give CS5 the baseline numbers it needs to prove its
throughput claims.

**Architecture:** One chaos knob that widens every timing window
(`DEBUG_HZ`), one that forces every allocation-failure path
(`DEBUG_PMMFAIL`), four userland stress binaries, and one kernel
selftest that proves the rank checker still catches inversions as ranks
are added. Everything is debug-build-only or a `REQUIRED_MARKERS` test;
nothing changes shipping behaviour.

**Tech Stack:** C (freestanding), x86-64, GNU Make, headless QEMU,
`tools/gauntlet.sh` (CONC=3), CS1's `DEBUG_HEAP`.

**Spec:** `docs/superpowers/specs/2026-09-01-concurrency-and-scaling-design.md` (CS3)

## Global Constraints

- **No host unit tests.** Every test is a kernel selftest writing
  through `serial_write_string`, or a userland binary under `userland/`
  ending in a `[name] ALL PASSED` marker.
- **A new userland binary needs four Makefile edits**, not one: the
  build rule, the `$(DISK_IMG)` dependency list, an `mcopy` line, the
  INITTAB `spawn` line, and the `REQUIRED_MARKERS` entry. A binary that
  builds but is never spawned passes silently by never running. Copy
  the pattern from `POLLTRUNC`/`TLBSTORM` (CS2) — `grep -n TLBSTORM
  Makefile` shows all five sites.
- **The gauntlet is the bar:** `tools/gauntlet.sh 15 3` →
  `PGAUNTLET PASSED: 15/15`. Tasks that touch memory or lifetime also
  run `GAUNTLET_MAKEFLAGS=DEBUG_HEAP=1 tools/gauntlet.sh 15 3`.
- **Every detector gets proved by making it fire**, then the proof is
  reverted. CS0 shipped two tests that passed for the wrong reason
  before this rule; CS2 found that a *deterministic* injection often
  cannot discriminate, so prefer injecting the **real** failure mode.
- **Stress selftests that need other CPUs run before
  `spawn("/SBIN/INIT.ELF")`** in `kmain`, beside `vt_stress_selftest`
  and `waitq_churn_selftest`. Running them after it makes them race the
  userland workload — CS2 hit exactly that, with `vt_stress` clearing
  the screen under TERM's render check.

## What CS3 does not contain, and why

- **`mmapracer` is already covered.** CS2's `userland/tlbstorm.c` runs
  4 processes × 150 rounds of mmap → touch → mprotect → verify →
  munmap against the shootdown, and asserts contents survive
  `mprotect` and frames come back. A separate binary would add
  overlapping-range cases only; fold those into `tlbstorm` if CS5's
  `vma` work makes them interesting, rather than duplicating it now.
- **`rankinvert` is partly done.** CS0 Task 6 added an adversarial
  probe for the four ranks it converted. Task 7 below generalises it to
  every defined rank rather than writing a new test.

---

## Task 1: `DEBUG_HZ` — widen every timing window

**Files:**
- Modify: `Makefile` (flag, beside `DEBUG_HEAP`/`DEBUG_LOCKSTAT`)
- Modify: `kernel/drivers/char/timer.c`

**The design constraint that shapes this.** The obvious implementation —
program the LAPIC faster — breaks the clock. `timer_ticks()` is the
kernel's time base: `rtc_boot_epoch() + ticks/100` anchors
`CLOCK_REALTIME`, `deadline_from_ms` divides by 10 for poll timeouts,
`nanosleep` blocks on it, and `TICKS_PER_LOG 100` assumes one log per
second. Multiply the tick rate and every one of those silently
mis-measures time.

So: **fire the interrupt N times more often, but advance the tick
counter every Nth interrupt.** Preemption points multiply; wall-clock
math is untouched. That is exactly the property the chaos knob wants —
more chances to be interrupted between any two instructions, with no
other observable change.

- [ ] **Step 1: Add the flag**

In `Makefile`, after the `DEBUG_LOCKSTAT` block:

```make
# Fire the LAPIC timer N times faster while keeping timer_ticks() at
# 100 Hz, so preemption windows widen without disturbing the clock.
ifdef DEBUG_HZ
CFLAGS += -DNEOOS_DEBUG_HZ=$(DEBUG_HZ)
endif
```

- [ ] **Step 2: Divide the period, divide the counting**

In `kernel/drivers/char/timer.c`, in `timer_init_this_cpu`, divide the
periodic count:

```c
void timer_init_this_cpu(void) {
    this_cpu()->timeslice_remaining = TIMESLICE_TICKS;
#ifdef NEOOS_DEBUG_HZ
    // Interrupt NEOOS_DEBUG_HZ times more often. The handler still
    // advances timer_ticks() only once per NEOOS_DEBUG_HZ interrupts,
    // so every consumer of the tick counter -- CLOCK_REALTIME's anchor,
    // poll deadlines, nanosleep, TICKS_PER_LOG -- keeps measuring real
    // time. Only the number of preemption points changes.
    uint32_t count = lapic_ticks_per_10ms / NEOOS_DEBUG_HZ;
    if (count == 0) { count = 1; }
    lapic_timer_start_periodic(count, VECTOR_TIMER);
#else
    lapic_timer_start_periodic(lapic_ticks_per_10ms, VECTOR_TIMER);
#endif
}
```

In `timer_handler`, gate **only the clock block**. The handler's shape
makes this simpler than it first looks:

```c
void timer_handler(void) {
    struct cpu *c = this_cpu();
    c->timer_ticks_local++;

    if (c == &cpus[0]) {          // only the BSP advances the wall clock
        tick_count++;
        ... TICKS_PER_LOG logging ...
        waitq_timeout_tick();
    }
    if (!c->current) { return; }
    if (--c->timeslice_remaining == 0) { ... schedule(); }
}
```

Wrap the **inside** of the `c == &cpus[0]` branch:

```c
    if (c == &cpus[0]) {
#ifdef NEOOS_DEBUG_HZ
        // Only the BSP reaches here, so a plain static needs no
        // synchronisation. Advance the wall clock once per
        // NEOOS_DEBUG_HZ interrupts, so tick_count, TICKS_PER_LOG,
        // waitq_timeout_tick, poll deadlines and nanosleep all keep
        // measuring real time while the interrupt itself fires
        // NEOOS_DEBUG_HZ times more often.
        static unsigned subtick;
        if (++subtick >= NEOOS_DEBUG_HZ) {
            subtick = 0;
#endif
        tick_count++;
        ... the existing body, unchanged ...
        waitq_timeout_tick();
#ifdef NEOOS_DEBUG_HZ
        }
#endif
    }
```

**Do not gate anything below that branch.** The `timeslice_remaining`
countdown and its `schedule()` must run on *every* interrupt on *every*
CPU — that is what multiplies preemption points, and it is the entire
purpose of the knob. An early `return` at the top of the handler would
skip both the timeout scan and preemption, achieving nothing.

Note there is no EOI to replicate: `timer_handler` does not send one
itself, so gating an inner block cannot strand the timer.

- [ ] **Step 3: Verify the clock did not move**

```bash
make clean-kernel && make DEBUG_HZ=8 test 2>&1 | tail -2
grep -E "^\[tier0test\]|^\[timer\] tick" build/serial.log | head -5
```

Expected: `PASS`, and `tier0test` — which asserts the clock advances and
`nanosleep` does not return early — still passes. **If `tier0test`
fails, the tick divider is wrong**, and that is the whole risk of this
task: it means wall-clock time is now being mis-measured.

- [ ] **Step 4: Verify preemption actually increased**

```bash
make clean-kernel && make DEBUG_LOCKSTAT=1 DEBUG_HZ=8 test > /dev/null 2>&1
grep "^\[lockstat\] 0x000000000000000f" build/serial.log
make clean-kernel && make DEBUG_LOCKSTAT=1 test > /dev/null 2>&1
grep "^\[lockstat\] 0x000000000000000f" build/serial.log
```

Rank `0xf` is `LOCK_RANK_RUNQUEUE`. Expected: its acquisition count is
substantially higher with `DEBUG_HZ=8` than without — that is the
evidence the knob does what it claims. Record both numbers in the
commit message. **If the counts are the same, the divider is skipping
the schedule() call as well as the tick**, which defeats the purpose:
the early return must happen *after* whatever drives preemption, or the
handler must be restructured so only the counter is gated.

- [ ] **Step 5: Run the existing suites at elevated rate**

```bash
make clean-kernel && make DEBUG_HZ=8 test 2>&1 | tail -2
make clean-kernel && make DEBUG_HEAP=1 DEBUG_HZ=8 test 2>&1 | tail -2
```

Expected: `PASS` both times. **A failure here is a real find** — it is a
race that exists today and that the normal tick rate simply does not
expose. Report it with the serial log rather than lowering the
multiplier.

- [ ] **Step 6: Gauntlet and commit**

```bash
tools/gauntlet.sh 15 3
GAUNTLET_MAKEFLAGS="DEBUG_HZ=8" tools/gauntlet.sh 15 3
git add Makefile kernel/drivers/char/timer.c
git commit -m "CS3: DEBUG_HZ multiplies preemption points, not the clock"
```

---

## Task 2: `forkstorm`

Targets `pid_alloc`, `proc_table`, `fd_table` and the exit path under
concurrent creation and destruction.

**Files:**
- Create: `userland/forkstorm.c`
- Modify: `Makefile` (five sites)

- [ ] **Step 1: Write the test**

`spawn()` and `fork()` both exist (`lib/include/unistd.h`). Model the
structure on `userland/tlbstorm.c`, which already does the
fork-children-and-check-status pattern correctly, including the
`WIFSIGNALED` case that CS2 had to fix.

The assertions that matter, since this is about tables rather than
throughput:

```c
// CS3: fork/exit storm against the pid, proc and fd tables.
//
//  - Every child's pid must be distinct from every other LIVE child's.
//    pid_alloc reuses freed ids from a free list, so a reuse bug shows
//    up as two live children sharing a pid.
//  - Every child must be reaped exactly once: a second waitpid for a
//    pid already reaped must fail, not hang or succeed.
//  - Free frames must not fall across the whole storm (the CS2
//    tolerance-band reasoning applies -- see tlbstorm.c).
#define GENERATIONS 40
#define WIDTH       8
```

Each generation forks `WIDTH` children that each open a couple of file
descriptors, write to a pipe, and exit; the parent records their pids,
waits for all of them, and checks the live-pid set for duplicates
before the next generation. Keep every child short — the point is
table churn, not compute.

Use `neoos_test_pmm_free()` around the whole storm with the same
`lost > 2000` band and the same comment reference as `tlbstorm.c`; do
not re-derive the reasoning, cite it.

- [ ] **Step 2: Wire it into the Makefile (five sites) and run**

```bash
make clean-kernel && make test 2>&1 | tail -2
grep -E "^\[forkstorm\]" build/serial.log
```

Expected: `[forkstorm] ALL PASSED`.

- [ ] **Step 3: Run it under the chaos knobs**

```bash
make clean-kernel && make DEBUG_HEAP=1 DEBUG_HZ=8 test 2>&1 | tail -2
grep -E "^\[forkstorm\]|heap. PANIC" build/serial.log
```

Expected: `ALL PASSED`, no heap panic. This combination — table churn,
poisoned heap, 8× preemption — is the most likely of anything in CS3 to
surface a real lifetime bug. **Report a failure here rather than
tuning it away.**

- [ ] **Step 4: Prove the duplicate-pid check can fire**

Temporarily make the check compare against a deliberately duplicated
entry: after recording the generation's pids, add
`pids[WIDTH - 1] = pids[0];   // TEMPORARY -- CS3 Task 2 detector proof`
before the duplicate scan.

```bash
make clean-kernel && make test 2>&1 | tail -3
grep -E "^\[forkstorm\] FAILED" build/serial.log
```

Expected: a duplicate-pid failure. **Then remove the line and rebuild.**

- [ ] **Step 5: Gauntlet and commit**

```bash
tools/gauntlet.sh 15 3
git add userland/forkstorm.c Makefile
git commit -m "CS3: forkstorm -- pid, proc and fd tables under churn"
```

---

## Task 3: `ptychurn`

**Files:**
- Create: `userland/ptychurn.c`
- Modify: `Makefile` (five sites)

`PTY_MAX` is 16 and the pool is a flat array; `pty_unref`'s `gone` check
drives teardown and `devfs_unregister`. CS0 made `pool_lock`
rank-checked, so an ordering mistake now panics instead of hiding.

- [ ] **Step 1: Write the test**

Follow `userland/ptytest.c` for the open pattern (`open("/dev/ptmx",
O_RDWR)`, then the `/dev/pts/N` path it names).

```c
// CS3: pty pool churn. Open and close ptys in a loop that crosses
// PTY_MAX repeatedly, racing master and slave teardown, to pressure
// pty_unref's refcounted `gone` path and the devfs registration it
// drives. Crossing the ceiling matters: the 17th open must fail
// cleanly with -ENFILE and must not corrupt the pool for the next
// round. CS4 removes the ceiling; this test is what proves the removal
// did not break teardown.
#define ROUNDS 60
```

Each round: open as many ptys as succeed (expect to hit `-ENFILE` at
`PTY_MAX`), assert the failure is `ENFILE` rather than a crash or a
wrong errno, write and read through a couple of them to prove they
still work, then close them all in a **different order** from the one
they were opened in. Assert that after closing everything, a fresh
open succeeds again — that is the teardown actually returning slots.

- [ ] **Step 2: Wire in, run, and run under chaos**

```bash
make clean-kernel && make test 2>&1 | tail -2
grep -E "^\[ptychurn\]" build/serial.log
make clean-kernel && make DEBUG_HEAP=1 DEBUG_HZ=8 test 2>&1 | tail -2
grep -E "^\[ptychurn\]|heap. PANIC|lock. PANIC" build/serial.log
```

Expected: `ALL PASSED` both times, no panic. A `[lock] PANIC: rank
inversion` naming `pty-pool` would be a genuine CS0 follow-up finding —
report it with the held-stack line.

- [ ] **Step 3: Prove the ceiling assertion can fire**

Temporarily assert the 17th open *succeeds* instead of failing:
invert the check with a comment marking it, rebuild, confirm the test
reports a failure, then restore. This proves the test is reading the
ceiling rather than passing vacuously.

- [ ] **Step 4: Gauntlet and commit**

```bash
tools/gauntlet.sh 15 3
git add userland/ptychurn.c Makefile
git commit -m "CS3: ptychurn -- the pty pool across its ceiling"
```

---

## Task 4: `sigstorm`

Signal delivery racing process and thread exit — the regression suite
for the refcounting the SMP-lifetime milestone installed.

**Files:**
- Create: `userland/sigstorm.c`
- Modify: `Makefile` (five sites)

- [ ] **Step 1: Write the test**

`kill(pid, sig)` and `tkill(tid, sig)` are both in
`lib/include/signal.h`. `userland/sigtest.c` shows the established
handler-installation pattern; follow it.

```c
// CS3: signal delivery overlapping exit. The dangerous window is a
// signal sent to a process that is concurrently exiting: the sender
// looks the target up, and the target's last thread may be tearing the
// process down underneath it. That is exactly what proc_get/proc_put
// and thread_get/thread_put were installed for, and this is their
// regression suite -- not a test of a bug still open.
//
// Nothing here asserts the signal is DELIVERED: racing exit, it is
// legitimately lost. What is asserted is that kill() never returns a
// bogus errno, never kills the wrong process, and never faults the
// kernel.
#define ROUNDS 100
```

Each round: fork a child that exits almost immediately; the parent
fires several `kill(child, SIGUSR1)` calls at it without synchronising,
then reaps it. Accept `0` or `-ESRCH` from `kill` — both are correct
depending on whether the child had gone — and **fail on any other
errno**, which is the real assertion. Then confirm the parent itself is
still alive and its own pid unchanged, catching a kill that landed on
the wrong target after pid reuse.

- [ ] **Step 2: Run it, plain and under chaos**

```bash
make clean-kernel && make test 2>&1 | tail -2
grep -E "^\[sigstorm\]" build/serial.log
make clean-kernel && make DEBUG_HEAP=1 DEBUG_HZ=8 test 2>&1 | tail -2
grep -E "^\[sigstorm\]|heap. PANIC|PANIC" build/serial.log
```

Expected: `ALL PASSED`, no panic.

- [ ] **Step 3: Prove it exercises the window**

A signal storm that always arrives after the child is gone proves
nothing. Add a temporary counter of how many `kill` calls returned `0`
(target still alive) versus `-ESRCH`, print it, and confirm **both are
non-zero** — that is the evidence the test straddles the exit window
rather than sitting on one side of it. Keep the counter in the shipped
test if it is cheap; a test that reports `killed=57 gone=43` tells a
future reader it is still straddling, where a bare `ALL PASSED` does
not.

- [ ] **Step 4: Gauntlet (both) and commit**

```bash
tools/gauntlet.sh 15 3
GAUNTLET_MAKEFLAGS=DEBUG_HEAP=1 tools/gauntlet.sh 15 3
git add userland/sigstorm.c Makefile
git commit -m "CS3: sigstorm -- signals racing process exit"
```

---

## Task 5: `pollstorm` — quantify the thundering herd

This one exists to produce a **number**, not a pass. CS5.2 replaces the
global `poll_broadcast` with per-object registration and has to prove it
helped.

**Files:**
- Modify: `kernel/sync/waitq.c` (a counter)
- Modify: `kernel/syscall/syscall_internal.h`, `kernel/syscall/sys_misc.c` (a test hook)
- Modify: `lib/syscall.c`, `lib/include/neoos_test.h` (the wrapper)
- Create: `userland/pollstorm.c`
- Modify: `Makefile` (five sites)

- [ ] **Step 1: Count broadcast wakeups in the kernel**

`sys_poll.c`'s header comment already describes the design: every
`poll`/`select` caller sleeps on one global waitq and is woken by *any*
readiness change anywhere. Add two counters next to that waitq in
`kernel/sync/waitq.c`:

```c
// CS3: the thundering-herd baseline. Every readiness change wakes every
// poll sleeper, so wakeups grow as O(sleepers x events) where a
// per-object design would be O(interested). Counting both halves turns
// "this is a scaling concern" into a ratio CS5.2 has to move.
static volatile uint64_t poll_events;    // readiness changes broadcast
static volatile uint64_t poll_wakeups;   // sleepers actually woken
```

Increment `poll_events` once per broadcast and `poll_wakeups` once per
sleeper woken, inside the existing broadcast path. Expose both through
one test hook, `TESTHOOK_POLL_STATS` (returning `(events << 32) |
wakeups`, both of which fit), with a `neoos_test_poll_stats()` wrapper —
follow `TESTHOOK_PMM_FREE` from CS2, which is the same shape.

- [ ] **Step 2: Write the measurement**

```c
// CS3: M pollers on disjoint fd sets, one driver making a single fd
// ready at a fixed rate. Each event concerns exactly ONE poller, so a
// per-object design would wake 1. Report the measured ratio; do not
// assert a bound on it, because the current design's whole point is
// that the ratio is bad. This is CS5.2's before-number.
#define POLLERS 6
#define EVENTS  50
```

Fork `POLLERS` children, each blocking in `poll()` on its own pipe. The
parent writes one byte to one child's pipe at a time, `EVENTS` times,
reaping as it goes. Read the counters before and after, and print:

```
[pollstorm] events=50 wakeups=300 ratio=6.0 (ideal 1.0 with POLLERS=6)
```

Print the ratio as integer tenths (`ratio=%d.%d`) — libneoos's `printf`
has no `%f`, and CS2 already found it has no `%ld` either.

Assert only that `events > 0 && wakeups >= events`, so the test fails if
the counters are not wired rather than if the ratio is bad.

- [ ] **Step 3: Run and record the number**

```bash
make clean-kernel && make test 2>&1 | tail -2
grep -E "^\[pollstorm\]" build/serial.log
```

**Copy the printed ratio into the commit message and into the spec's
CS5.2 section.** A baseline nobody wrote down is not a baseline.

- [ ] **Step 4: Gauntlet and commit**

```bash
tools/gauntlet.sh 15 3
git add kernel/sync/waitq.c kernel/syscall/syscall_internal.h kernel/syscall/sys_misc.c lib/syscall.c lib/include/neoos_test.h userland/pollstorm.c Makefile
git commit -m "CS3: pollstorm -- measure the poll_broadcast thundering herd"
```

---

## Task 6: `pmm_alloc` failure injection, and `faultflood`

Every `if (!frame) return ...` path in `vma.c`, `tlb.c` and
`fd_table.c` is written but never executed, because allocation does not
fail in a 128 MiB VM running small tests.

**Files:**
- Modify: `Makefile` (`DEBUG_PMMFAIL`)
- Modify: `kernel/mm/pmm.c`
- Create: `userland/faultflood.c`
- Modify: `Makefile` (five sites)

- [ ] **Step 1: Add the injector**

In `kernel/mm/pmm.c`, at the top of `pmm_alloc`:

```c
#ifdef NEOOS_DEBUG_PMMFAIL
// Fail one allocation in NEOOS_DEBUG_PMMFAIL, deterministically by
// count. Deterministic rather than random on purpose: a failure that
// cannot be reproduced from the build flag alone is a bug report nobody
// can act on. The counter is global and unlocked -- exactness does not
// matter, only that failures keep arriving.
static volatile uint64_t pmmfail_n;
if ((__atomic_add_fetch(&pmmfail_n, 1, __ATOMIC_RELAXED)
     % NEOOS_DEBUG_PMMFAIL) == 0) {
    return 0;
}
#endif
```

and the Makefile flag beside the others:

```make
ifdef DEBUG_PMMFAIL
CFLAGS += -DNEOOS_DEBUG_PMMFAIL=$(DEBUG_PMMFAIL)
endif
```

- [ ] **Step 2: Find out what actually survives**

```bash
make clean-kernel && make DEBUG_PMMFAIL=1000 test 2>&1 | tail -3
grep -E "PANIC|\[exception\]|FAILED" build/serial.log | head
```

**This is the interesting step of CS3 and it is a survey, not a
pass/fail.** One allocation in 1000 failing will exercise error paths
that have never run. Expect findings. For each, record whether the
kernel handled it (a syscall returned `-ENOMEM`, a process died
cleanly) or mishandled it (a panic, a fault, a silent wrong result).

Then tighten and repeat: `DEBUG_PMMFAIL=200`, then `50`. Record at
which rate the system stops booting — that number is itself useful.

**Fix what you find in this task only if it is a null-check that is
plainly missing.** Anything structural (a path that cannot fail
gracefully without a redesign) gets recorded in the spec for CS4/CS5
rather than fixed here, with the injection rate that reproduces it.

- [ ] **Step 3: Write `faultflood`**

```c
// CS3: demand paging under allocation pressure. Touch far more distinct
// pages than the machine can back, so vma_fault_locked's
// `if (!frame) return 0;` path -- which becomes a SIGSEGV -- runs for
// real rather than only in theory. Under DEBUG_PMMFAIL it runs
// constantly.
//
// The assertion is not "every touch succeeds". It is that the process
// dies by SIGSEGV or the mapping fails with a proper errno, and that
// the KERNEL survives: no panic, no fault, and the machine still runs
// every later test.
```

Map a large anonymous region, install a `SIGSEGV` handler that records
and exits cleanly, and walk it page by page until either the walk
completes or the handler fires. Either outcome passes; a kernel panic
does not, and the gauntlet catches that independently.

- [ ] **Step 4: Run the combination**

```bash
make clean-kernel && make DEBUG_PMMFAIL=200 test 2>&1 | tail -2
grep -E "^\[faultflood\]|PANIC" build/serial.log
```

- [ ] **Step 5: Gauntlet and commit**

Run the plain gauntlet (the injector is off by default), then:

```bash
tools/gauntlet.sh 15 3
git add Makefile kernel/mm/pmm.c userland/faultflood.c
git commit -m "CS3: pmm_alloc failure injection, and faultflood"
```

---

## Task 7: `rankinvert` — every rank, not four

CS0 Task 6 proved the checker rejects a descending acquire at the four
ranks it converted. As ranks are added, that check does not grow with
them.

**Files:**
- Modify: `kernel/sync/lock.c` (generalise the CS0 probe)

- [ ] **Step 1: Generalise the probe**

Replace the hand-listed CS0 block with a loop over a table of every
defined rank, so adding a rank without adding it here is visible:

```c
// CS3: the checker itself, over every defined rank rather than the four
// CS0 happened to convert. For each rank, holding a lock of that rank
// must make every LOWER rank illegal, the SAME rank illegal, and the
// next higher rank legal. A new LOCK_RANK_* that is not in this table
// is caught by the count assertion below.
static const uint8_t all_ranks[] = {
    LOCK_RANK_PROCTABLE, LOCK_RANK_PROCESS, LOCK_RANK_THREAD,
    LOCK_RANK_MM, LOCK_RANK_MOUNTTABLE, LOCK_RANK_VNODEHASH,
    LOCK_RANK_VNODE, LOCK_RANK_BLOCKDEV, LOCK_RANK_DRIVER,
    LOCK_RANK_FUTEX, LOCK_RANK_PIPE, LOCK_RANK_SOCKTABLE,
    LOCK_RANK_SOCKET, LOCK_RANK_TIMEOUT, LOCK_RANK_WAITQ,
    LOCK_RANK_RUNQUEUE, LOCK_RANK_FDTABLE, LOCK_RANK_HEAP,
    LOCK_RANK_PMM, LOCK_RANK_SIGQUEUE, LOCK_RANK_TLB, LOCK_RANK_INPUT,
    LOCK_RANK_RAND, LOCK_RANK_VT, LOCK_RANK_PTY, LOCK_RANK_DEVFS,
    LOCK_RANK_FBCON, LOCK_RANK_SERIAL,
};
```

Note `LOCK_RANK_TTY == LOCK_RANK_DRIVER` deliberately, so the table
lists `DRIVER` once and the equal-rank case covers TTY.

For each entry: `spin_init` a probe at that rank, acquire it, and
assert `lock_rank_ok` is false for every strictly lower rank in the
table and for the rank itself, and true for every strictly higher one.
Release before moving on. `lock_rank_ok` reports legality without
entering the panic path, which is why this can loop at all.

Add a count check so the table cannot silently fall behind:

```c
    // lock.h has 29 LOCK_RANK_* defines but 28 distinct values, since
    // LOCK_RANK_TTY is deliberately LOCK_RANK_DRIVER. Bump this when
    // adding a rank, and add it to all_ranks above.
    if (sizeof(all_ranks) / sizeof(all_ranks[0]) != 28) {
        serial_write_string("[lock] selftest FAILED: rank table out of date\n");
        return;
    }
```

- [ ] **Step 2: Run it**

```bash
make clean-kernel && make test 2>&1 | tail -2
grep -E "^\[lock\]" build/serial.log
```

Expected: `[lock] selftest passed`.

- [ ] **Step 3: Prove it fires**

Temporarily add a bogus rank to the table that duplicates an existing
one (e.g. list `LOCK_RANK_HEAP` twice). Expected: the equal-rank
assertion fails. Remove it and rebuild.

- [ ] **Step 4: Gauntlet and commit**

```bash
tools/gauntlet.sh 15 3
git add kernel/sync/lock.c
git commit -m "CS3: prove the rank checker over every defined rank"
```

---

## Task 8: Close out CS3

**Files:**
- Modify: `docs/superpowers/specs/2026-09-01-concurrency-and-scaling-design.md` (CS3 section)
- Modify: `docs/debugging-tools.md` (the two new knobs)

- [ ] **Step 1: Document the knobs**

Add `DEBUG_HZ` and `DEBUG_PMMFAIL` to `docs/debugging-tools.md` beside
`DEBUG_HEAP` and `DEBUG_LOCKSTAT`: what each does, why `DEBUG_HZ`
divides the tick counter rather than raising it, and the
`DEBUG_PMMFAIL` rates at which the system still boots.

- [ ] **Step 2: Record the results in the spec**

Mark CS3 done, and record specifically:

- **`pollstorm`'s measured ratio**, as CS5.2's before-number.
- **What `DEBUG_PMMFAIL` found**, including "nothing" if the error
  paths all held. Note the rate at which booting stops.
- **Whether `DEBUG_HZ=8` surfaced anything** the normal rate does not.
- Which stress binaries found nothing — a green stress test is a real
  result once it has been shown capable of failing.

- [ ] **Step 3: Final verification**

```bash
make clean-kernel && make test 2>&1 | tail -2
make clean-kernel && make DEBUG_HEAP=1 DEBUG_HZ=8 test 2>&1 | tail -2
tools/gauntlet.sh 15 3
GAUNTLET_MAKEFLAGS=DEBUG_HEAP=1 tools/gauntlet.sh 15 3
```

- [ ] **Step 4: Commit**

```bash
git add docs
git commit -m "CS3 done; record the chaos knobs and the baselines"
```

---

## Notes for the executor

- **Task 1 first.** Tasks 2-6 all want to run under `DEBUG_HZ`, and its
  tick-divider design is the one piece here that can break the clock for
  everything else.
- **Tasks 2-5 are independent** of each other and may be reordered.
  Task 6 is the most likely to find real bugs and the most likely to
  need judgement about what to fix here versus record for later.
- **Five Makefile sites per new binary.** `grep -n TLBSTORM Makefile`
  lists them.
- **libneoos `printf` has no `%f` and no `%ld`.** Use `%d` with `(int)`
  casts; print ratios as integer tenths.
- **Do not lower a threshold or a multiplier to make a test pass.** If
  `DEBUG_HZ=8` or `DEBUG_PMMFAIL=200` breaks something, that is the
  milestone working.
