# CS2 — Named regressions — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix the bugs the code's own comments already describe, each
with a regression test that fails before the fix and passes after.

**Architecture:** Four independent fixes. The PID radix tree turns out
to be dead code and is deleted rather than repaired. `select()`'s silent
truncation is fixed by sizing the descriptor array to the caller's
request, which pulls one row of CS4 forward because the correctness fix
is not separable from it. The waitq double-free and the TLB shootdown
get stress tests built on CS1's poisoned heap and a new test-hook that
exposes the free-frame count.

**Tech Stack:** C (freestanding), x86-64, GNU Make, headless QEMU,
`tools/gauntlet.sh` (CONC=3), CS1's `DEBUG_HEAP`.

**Spec:** `docs/superpowers/specs/2026-09-01-concurrency-and-scaling-design.md` (CS2)

## Global Constraints

- **No host unit tests.** Every test is a kernel selftest writing
  through `serial_write_string`, or a userland binary under
  `userland/` ending in a `[name] ALL PASSED` marker wired into the
  Makefile's `REQUIRED_MARKERS` **and** into `build/disk-src/INITTAB`
  via the Makefile's INITTAB generation.
- **The gauntlet is the bar.** After each task, `tools/gauntlet.sh 15 3`
  must report `PGAUNTLET PASSED: 15/15`. Tasks 3 and 4 additionally run
  `GAUNTLET_MAKEFLAGS=DEBUG_HEAP=1 tools/gauntlet.sh 15 3`, since their
  whole point is corruption that only a poisoned heap localises.
- **A regression test must fail before the fix.** Each task has an
  explicit step that observes the failure. A test that has only ever
  been green is not evidence — CS0 produced two tests that passed for
  the wrong reason before this rule was applied.
- **Lock ranks are enforced**; any new lock needs a `LOCK_RANK_*` slot
  with a written rationale.
- **The ABI is not ours.** Task 2 changes `select()`/`poll()`
  behaviour a program can observe, so it must move *toward* Linux, and
  any residual divergence goes in `docs/stdlib.md`.

## What CS2 no longer contains, and why

Three of the spec's seven CS2 items need no work here:

- **CS2.3 (rank-checker coverage) is already done.** CS0 Task 6 added
  the adversarial selftest asserting the checker rejects a descending
  acquire at each converted rank (VT, PTY, DEVFS, RAND) — commit
  `6b12e35`.
- **CS2.7 (VT-switch locking) is already done.** CS0 Task 5 found it
  real and fixed it — commit `e1868ec`.
- **CS2.5 (`fd_table_dup2`) stays deferred.** `clone()` still does not
  exist (`grep -rn sys_clone kernel/syscall/` returns nothing), so the
  race is unreachable. It remains a recorded blocking pre-condition on
  the `clone()` milestone. Task 5 re-states it; no code changes.

---

## Task 1: Delete the dead PID radix tree

**The spec's headline CS2 item dissolved on inspection.** The lockless
`pid_lookup` is real code with a real race written into it, but it is
**unreachable**: `pid_insert`, `pid_remove` and `pid_lookup` have no
callers anywhere in the tree, so `alloc->root` is permanently `NULL`
and `pid_lookup_internal` returns on its first line. Verified with:

```bash
grep -rn "pid_insert\|pid_remove\|pid_lookup" kernel --include="*.c" --include="*.h" | grep -v "^kernel/sched/pid_alloc"
```

which returns nothing. Real process lookup is `proc_table_lookup` — a
bucketed hash with per-bucket locks at `LOCK_RANK_PROCTABLE`, already
refcounted by the SMP-lifetime milestone.

So the fix is deletion, not refcounting. What survives is the part
that is actually used: `pid_alloc()` / `pid_free()`, a PID *number*
allocator over a free list and `next_pid`.

Two more dead functions go with it. `pid_alloc_specific()` is called
only by `proc_table_alloc_pid_zero()`, which is itself never called —
and which could never have worked, since `pid_alloc_specific` rejects
`pid <= 0` on its first line and is always passed `0`.

**Files:**
- Modify: `kernel/sched/pid_alloc.c` (delete the radix half)
- Modify: `kernel/sched/pid_alloc.h` (delete the declarations and `struct pid_radix_node`)
- Modify: `kernel/sched/proc_table.c:146-149`, `kernel/sched/proc_table.h:88` (delete `proc_table_alloc_pid_zero`)
- Test: `kernel/sched/pid_alloc.c` (a selftest for the contract that remains)

**Interfaces:**
- Removes: `pid_lookup`, `pid_insert`, `pid_remove`, `pid_alloc_specific`,
  `proc_table_alloc_pid_zero`, `struct pid_radix_node`, `PID_RADIX_BITS`,
  `PID_RADIX_SIZE`.
- Keeps unchanged: `pid_allocator_init`, `pid_alloc`, `pid_free`,
  `struct pid_allocator` (minus its `root` field), `struct pid_free_entry`.
- Produces: `void pid_alloc_selftest(void);` declared in
  `kernel/sched/pid_alloc.h`, called from `kmain` next to the other
  scheduler selftests.

- [ ] **Step 1: Write the test for the contract that remains**

The allocator's real contract is: never hand out the same PID twice
while it is outstanding, and reuse freed PIDs. Add to
`kernel/sched/pid_alloc.c`:

```c
void pid_alloc_selftest(void) {
    struct pid_allocator a;
    pid_allocator_init(&a);

    // Distinctness: a run of allocations with nothing freed must be
    // strictly increasing and never repeat.
    int ids[64];
    for (int i = 0; i < 64; i++) {
        ids[i] = pid_alloc(&a);
        if (ids[i] <= 0) {
            serial_write_string("[pid] selftest FAILED: allocation returned 0\n");
            return;
        }
        for (int j = 0; j < i; j++) {
            if (ids[j] == ids[i]) {
                serial_write_string("[pid] selftest FAILED: duplicate live pid\n");
                return;
            }
        }
    }

    // Reuse: a freed pid comes back before next_pid grows again.
    int freed = ids[10];
    pid_free(&a, freed);
    int reused = pid_alloc(&a);
    if (reused != freed) {
        serial_write_string("[pid] selftest FAILED: freed pid not reused\n");
        return;
    }

    // The allocator must survive being drained and refilled without
    // handing back a pid that is still outstanding.
    for (int i = 0; i < 64; i++) {
        if (ids[i] != freed) { pid_free(&a, ids[i]); }
    }
    pid_free(&a, reused);
    for (int i = 0; i < 64; i++) {
        int p = pid_alloc(&a);
        if (p <= 0) {
            serial_write_string("[pid] selftest FAILED: refill returned 0\n");
            return;
        }
    }

    serial_write_string("[pid] selftest passed\n");
}
```

Declare it in `kernel/sched/pid_alloc.h`:

```c
void pid_alloc_selftest(void);
```

and call it from `kernel/kernel.c`, next to the other scheduler
selftests (after `proc_table_init`'s selftest; find the spot with
`grep -n "proc_table\|_selftest();" kernel/kernel.c | head -20`).

- [ ] **Step 2: Run it against the current code**

```bash
make clean-kernel && make test 2>&1 | tail -2
grep -E "^\[pid\]" build/serial.log
```

Expected: `[pid] selftest passed`. **This test is expected to pass
before the change** — it documents the contract that must survive the
deletion, which is the point: Task 1 is a removal, and the test exists
to prove the removal changed nothing that mattered.

- [ ] **Step 3: Delete the radix tree**

From `kernel/sched/pid_alloc.c`, delete `pid_lookup_internal`,
`pid_insert_internal`, `pid_remove_internal`, `pid_lookup`,
`pid_insert`, `pid_remove` and `pid_alloc_specific` entirely. Remove
`alloc->root = 0;` from `pid_allocator_init`.

From `kernel/sched/pid_alloc.h`, delete `struct pid_radix_node`, the
`PID_RADIX_BITS` / `PID_RADIX_SIZE` defines, the `root` field of
`struct pid_allocator`, and the declarations of every function deleted
above.

Add a comment where the tree used to be, so the next reader does not
re-add it speculatively:

```c
// This allocator hands out PID *numbers* only. Process lookup by pid is
// proc_table_lookup()'s bucketed hash (kernel/sched/proc_table.c), which
// has per-bucket locks and refcounted results. A radix tree with
// insert/remove/lookup lived here until CS2 and was deleted: nothing
// ever called those three functions, so the tree was never populated,
// and its lookup path read shared nodes with no lock at all -- a race
// that was only ever unreachable by accident.
```

From `kernel/sched/proc_table.c`, delete `proc_table_alloc_pid_zero`;
from `kernel/sched/proc_table.h`, delete its declaration.

- [ ] **Step 4: Verify nothing referenced any of it**

```bash
make clean-kernel && make build 2>&1 | grep -E " error" ; echo "(no errors = nothing referenced the deleted API)"
grep -rn "pid_lookup\|pid_insert\|pid_remove\|pid_alloc_specific\|proc_table_alloc_pid_zero\|pid_radix" kernel userland lib 2>/dev/null
```

Expected: a clean build and no remaining references.

- [ ] **Step 5: Run the tests**

```bash
make test 2>&1 | tail -2
grep -E "^\[pid\]|^\[proc\]" build/serial.log
```

Expected: `PASS`, `[pid] selftest passed`.

- [ ] **Step 6: Gauntlet and commit**

```bash
tools/gauntlet.sh 15 3
git add kernel/sched/pid_alloc.c kernel/sched/pid_alloc.h kernel/sched/proc_table.c kernel/sched/proc_table.h kernel/kernel.c
git commit -m "CS2: delete the dead PID radix tree"
```

---

## Task 2: `select()` stops silently dropping ready fds

**Files:**
- Modify: `kernel/syscall/sys_poll.c`
- Create: `userland/polltrunc.c`
- Modify: `Makefile` (`REQUIRED_MARKERS` and the INITTAB spawn list)
- Modify: `docs/stdlib.md`

**Interfaces:**
- Produces: the `[polltrunc] ALL PASSED` marker.

**Deviation from the spec, deliberate:** the spec assigns the
correctness fix to CS2 and the dynamic array to CS4. They are not
separable. The bug *is* the fixed 16-entry array, and the only
behaviours available without removing it are "silently drop" (today) or
"return `-EINVAL`", which would break a shell just as badly and is not
what Linux does. So CS2 takes the dynamic array; CS4 keeps the rest of
that row (the `FD_TABLE_MAX` bound and `poll()`'s own cap review).

- [ ] **Step 1: Write the failing test**

Create `userland/polltrunc.c`. Model its structure on an existing test —
read `userland/polltest.c` first for the established marker style, exit
convention and helper usage, and follow it.

The test opens enough pipes to describe more than 16 *interesting*
descriptors, makes them all readable, and asserts `select()` reports
every one:

```c
// CS2.2: select() collected into a fixed 16-entry array while accepting
// nfds up to FD_SETSIZE (1024), so it silently dropped every ready fd
// past the first 16 it encountered -- no error, no truncation flag.
#define NPIPES 20

int main(void) {
    int rfd[NPIPES];
    int maxfd = 0;
    for (int i = 0; i < NPIPES; i++) {
        int fds[2];
        if (pipe(fds) != 0) { fail("pipe"); }
        // make it readable so select() must report it
        if (write(fds[1], "x", 1) != 1) { fail("write"); }
        rfd[i] = fds[0];
        if (rfd[i] > maxfd) { maxfd = rfd[i]; }
    }

    fd_set r;
    FD_ZERO(&r);
    for (int i = 0; i < NPIPES; i++) { FD_SET(rfd[i], &r); }

    struct timeval tv = { 0, 0 };
    int n = select(maxfd + 1, &r, 0, 0, &tv);
    if (n != NPIPES) {
        printf("[polltrunc] FAILED: select reported %d of %d ready\n", n, NPIPES);
        return 1;
    }
    for (int i = 0; i < NPIPES; i++) {
        if (!FD_ISSET(rfd[i], &r)) {
            printf("[polltrunc] FAILED: fd %d ready but not reported\n", rfd[i]);
            return 1;
        }
    }
    printf("[polltrunc] 20 simultaneously-ready fds all reported\n");
    printf("[polltrunc] ALL PASSED\n");
    return 0;
}
```

Wire it into the Makefile: add `"[polltrunc] ALL PASSED"` to
`REQUIRED_MARKERS`, and add `spawn /BIN/POLLTRUNC.ELF` to the INITTAB
generation next to the other spawns. Copy the exact pattern an existing
entry uses — `grep -n "POLLTEST" Makefile` shows every place a test
binary must be named.

- [ ] **Step 2: Run it to see it fail**

```bash
make clean-kernel && make test 2>&1 | tail -3
grep -E "^\[polltrunc\]" build/serial.log
```

Expected: `[polltrunc] FAILED: select reported 16 of 20 ready` — the
silent truncation, made loud.

- [ ] **Step 3: Size the array to the request**

In `kernel/syscall/sys_poll.c`, both `sys_poll` and `sys_select` use a
fixed `struct pollfd pfd[POLL_MAX_FDS];` on the kernel stack. Replace
the `select()` path's array with a `kmalloc`'d one sized to the number
of interesting fds the caller actually described.

Read the current implementation first:

```bash
sed -n '1,140p' kernel/syscall/sys_poll.c
```

Then, in `sys_select`, count the interesting fds in the caller's sets
before collecting, allocate for exactly that many, and drop the
`n < POLL_MAX_FDS` clause from the collection loop so nothing is
skipped. Bound the allocation by `FD_SETSIZE` — `nfds` is already
rejected above that, so a caller cannot drive an unbounded allocation.
Free the array on every exit path, including the error and
timeout paths.

Leave `poll()`'s own `POLL_MAX_FDS` array alone; CS4 revisits it. If
`poll()` shares the collection helper with `select()`, give the helper
the array and length as parameters rather than duplicating it.

- [ ] **Step 4: Run the test to verify it passes**

```bash
make test 2>&1 | tail -2
grep -E "^\[polltrunc\]" build/serial.log
```

Expected: `[polltrunc] 20 simultaneously-ready fds all reported` and
`[polltrunc] ALL PASSED`.

- [ ] **Step 5: Check for an allocation leak**

`select()` now allocates per call, and the console polls constantly. Run
with the poisoned heap, which panics on double free and use-after-free:

```bash
make clean-kernel && make DEBUG_HEAP=1 test 2>&1 | tail -2
grep -E "heap. PANIC" build/serial.log ; echo "(no output = no heap fault)"
```

Expected: `PASS` and no heap panic. A missing `kfree` on the timeout
path will not show here — it shows as a frame leak — so also confirm
`[polltest] ALL PASSED` still appears, since it exercises the blocking
path repeatedly.

- [ ] **Step 6: Document**

Add a `docs/stdlib.md` note: `select()` now reports all ready
descriptors up to `FD_SETSIZE`, matching Linux; `poll()`'s `nfds` is
still capped at `POLL_MAX_FDS` and returns `-EINVAL` above it, which is
a **divergence from Linux** recorded until CS4 removes it.

- [ ] **Step 7: Gauntlet and commit**

```bash
tools/gauntlet.sh 15 3
git add kernel/syscall/sys_poll.c userland/polltrunc.c Makefile docs/stdlib.md
git commit -m "CS2: select() reports every ready fd, not the first 16"
```

---

## Task 3: A regression test for the waitq double-free

`waitq.c`'s `selftest_thread` comment describes a real bug found once,
by luck: a thread freeing itself while still executing on its own
about-to-be-freed kernel stack, with a timer interrupt on another CPU
running the same free path via `idle_entry`'s kzombies drain. The fix
is in place; nothing forces the race to recur.

**Files:**
- Modify: `kernel/sync/waitq.c` (add the churn selftest)
- Modify: `kernel/kernel.c` (call it after the APs are up)

**Interfaces:**
- Produces: `void waitq_churn_selftest(void);` declared in
  `kernel/sync/waitq.h`.

- [ ] **Step 1: Write the test**

Short-lived kernel threads, spawned and exited back-to-back across every
online CPU, to widen the exit-vs-drain window. Add to
`kernel/sync/waitq.c`:

```c
// CS2.4: the exit-vs-drain race. A thread that frees itself is still
// running on its own kernel stack; idle_entry's kzombies drain can free
// the same thread again from another CPU. Found once from a free_list
// pointer of 0xfffffffc and fixed, but never forced to recur. Hundreds
// of short-lived threads across every CPU make the window likely rather
// than lucky; under DEBUG_HEAP a second free of the same slot panics
// immediately instead of corrupting the free list.
#define CHURN_ROUNDS 400

static volatile int churn_live;

static void churn_thread(void) {
    __atomic_fetch_sub(&churn_live, 1, __ATOMIC_RELEASE);
    thread_exit_self(0);
}

void waitq_churn_selftest(void) {
    int online = smp_online_count();
    churn_live = 0;
    for (int i = 0; i < CHURN_ROUNDS; i++) {
        __atomic_fetch_add(&churn_live, 1, __ATOMIC_ACQUIRE);
        if (!thread_alloc_kernel_on(churn_thread, i % online)) {
            __atomic_fetch_sub(&churn_live, 1, __ATOMIC_RELEASE);
            serial_write_string("[waitq] churn SKIPPED: thread_alloc failed\n");
            return;
        }
    }
    // Bounded wait: if the drain double-frees, DEBUG_HEAP panics before
    // we get here; if a thread is lost, this must not hang the boot.
    for (uint64_t spins = 0; churn_live > 0 && spins < 2000000000ULL; spins++) {
        __asm__ volatile ("pause");
    }
    if (churn_live > 0) {
        serial_write_string("[waitq] churn FAILED: threads never exited\n");
        return;
    }
    serial_write_string("[waitq] churn passed\n");
}
```

`waitq.c` today calls `thread_alloc_kernel()` but not
`thread_alloc_kernel_on()` or `smp_online_count()`, so add
`#include "smp/smp.h"` if it is not already there and confirm
`sched/proc.h` is included for the `_on` variant.

Declare it in `kernel/sync/waitq.h` and call it from `kernel/kernel.c`
**after `smp_start_aps()`** — next to `vt_stress_selftest()`, which has
the same requirement. Calling it before the APs exist queues threads on
CPUs that never run them and hangs the boot; CS0 Task 5 hit exactly
that.

- [ ] **Step 2: Run it, production and poisoned**

```bash
make clean-kernel && make test 2>&1 | tail -2
grep -E "^\[waitq\]" build/serial.log
make clean-kernel && make DEBUG_HEAP=1 test 2>&1 | tail -2
grep -E "^\[waitq\]|heap. PANIC" build/serial.log
```

Expected: `[waitq] churn passed` in both, no heap panic.

**If a heap panic appears here, that is a real find** — the fix in
`waitq.c` is incomplete, and this test has just reproduced on purpose
what previously happened once by accident. Report it with the serial
log and the panic's slot address rather than weakening the test.

- [ ] **Step 3: Prove the test can actually observe the bug**

A green churn test is only evidence if the test would catch a double
free. Temporarily restore the second free the comment describes — read
the comment at `kernel/sync/waitq.c:320-330` to find the exact line that
was commented out, uncomment it, and run under `DEBUG_HEAP=1`:

```bash
make clean-kernel && make DEBUG_HEAP=1 test 2>&1 | tail -3
grep -E "double free|heap. PANIC" build/serial.log
```

Expected: `[heap] PANIC: double free of slot=...`. **Then re-comment
the line and rebuild.** If the panic does *not* appear, the churn test
is not reaching the drain path and needs more rounds or a raised tick
rate before it counts as cover.

- [ ] **Step 4: Gauntlet (both builds) and commit**

```bash
tools/gauntlet.sh 15 3
GAUNTLET_MAKEFLAGS=DEBUG_HEAP=1 tools/gauntlet.sh 15 3
git add kernel/sync/waitq.c kernel/sync/waitq.h kernel/kernel.c
git commit -m "CS2: force the waitq exit-vs-drain race with kernel-thread churn"
```

---

## Task 4: TLB shootdown under contention, with frame accounting

**Files:**
- Modify: `kernel/syscall/syscall_internal.h` (a new test-hook code)
- Modify: `kernel/syscall/sys_misc.c` (`sys_test_hook`)
- Create: `userland/tlbstorm.c`
- Modify: `Makefile` (`REQUIRED_MARKERS`, INITTAB)

**Interfaces:**
- Produces: `TESTHOOK_PMM_FREE 4` — returns `pmm_free_frame_count()`.
  Only in `-DNEOOS_TEST_HOOKS` builds, like the three codes beside it.
- Produces: the `[tlbstorm] ALL PASSED` marker.

- [ ] **Step 1: Expose the free-frame count to the test**

The assertion "no deferred frame was leaked or double-freed" needs the
count from userland. The test-hook syscall already exists for exactly
this kind of thing. In `kernel/syscall/syscall_internal.h`, beside the
existing codes:

```c
#define TESTHOOK_PMM_FREE     4   // returns pmm_free_frame_count()
```

and in `sys_test_hook` in `kernel/syscall/sys_misc.c`, beside the other
cases:

```c
    case TESTHOOK_PMM_FREE:
        return (int64_t)pmm_free_frame_count();
```

adding `#include "mm/pmm.h"` if it is not already included.

- [ ] **Step 2: Write the test**

Create `userland/tlbstorm.c`. `clone()` does not exist, so the
concurrency comes from **processes**, not threads — which still
exercises the shootdown, because `shootdown_busy` is a single global
serialising every address-space mutation on the machine, and
`munmap`/`mprotect` send IPIs to every CPU regardless of which address
space they belong to.

Read `userland/mmaptest.c` first: it does **not** call a libc `mmap` —
it uses a local `mmap_raw()` helper and defines `MAP_ANONYMOUS 0x20`
itself. Use that same helper and check its return against the raw error
convention it already uses, not `MAP_FAILED`. The sketch below is
written in POSIX shape for readability; translate it to `mmap_raw`:

```c
// CS2.6: tlb_shootdown's shootdown_busy is an unchecked global spin on
// every mmap/munmap/mprotect. Two things to assert under real
// contention: the 50,000,000-spin timeout path never fires (that log
// line outside a deliberately wedged CPU means correctness has quietly
// degraded to "continuing anyway"), and frames come back.
#define CHILDREN  4
#define ROUNDS    200
#define REGION    (16 * 4096)

static void hammer(void) {
    for (int r = 0; r < ROUNDS; r++) {
        char *p = mmap(0, REGION, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { _exit(2); }
        // touch every page so the mapping is really backed
        for (int i = 0; i < REGION; i += 4096) { p[i] = (char)(r + i); }
        if (mprotect(p, REGION, PROT_READ) != 0) { _exit(3); }
        for (int i = 0; i < REGION; i += 4096) {
            if (p[i] != (char)(r + i)) { _exit(4); }   // mprotect ate the contents
        }
        if (munmap(p, REGION) != 0) { _exit(5); }
    }
    _exit(0);
}
```

`main` records the free-frame count via the test hook, forks `CHILDREN`
children into `hammer()`, waits for all of them, checks every exit
status is 0, then re-reads the count and asserts it returned to the
starting value. Print the before/after numbers on failure — a leak and
a double-free look different and the numbers say which.

Note the frame count will not match exactly if anything else in the
system allocates concurrently, so compare against the count taken
*after* a warm-up round in the parent, and allow the test to state the
delta rather than assert bit-equality if the system proves noisy. Decide
which on the evidence of Step 3, and record the choice in a comment.

Wire the marker and the INITTAB entry in exactly as Task 2 does.

- [ ] **Step 3: Run it**

```bash
make clean-kernel && make test 2>&1 | tail -3
grep -E "^\[tlbstorm\]|shootdown timed out" build/serial.log
```

Expected: `[tlbstorm] ALL PASSED` and **no** `[tlb] shootdown timed
out; continuing`.

**Either failure is a real find, not a test problem:**
- a timeout line means the shootdown wedges under ordinary contention;
- a frame-count mismatch means the deferred-free path leaks or
  double-frees.

Report either with the numbers rather than loosening the assertion. Note
that commit `7313b77` ("mm: reclaim deferred frames, keep mprotect's
pages, single-thread exec") touched these paths recently and may already
cover part of the accounting — if the count is clean first try, say so;
that is the useful result.

- [ ] **Step 4: Run it under the poisoned heap**

```bash
make clean-kernel && make DEBUG_HEAP=1 test 2>&1 | tail -2
grep -E "heap. PANIC" build/serial.log ; echo "(no output = clean)"
```

`tlb.c` allocates overflow nodes with `kmalloc`, so this is the run that
would catch a use-after-free in the deferred-free queue.

- [ ] **Step 5: Gauntlet (both builds) and commit**

```bash
tools/gauntlet.sh 15 3
GAUNTLET_MAKEFLAGS=DEBUG_HEAP=1 tools/gauntlet.sh 15 3
git add kernel/syscall/syscall_internal.h kernel/syscall/sys_misc.c userland/tlbstorm.c Makefile
git commit -m "CS2: stress the TLB shootdown and assert frames come back"
```

---

## Task 5: Close out CS2

**Files:**
- Modify: `docs/superpowers/specs/2026-09-01-concurrency-and-scaling-design.md` (CS2 section)
- Modify: `docs/abi-compatibility.md`

- [ ] **Step 1: Record the results in the spec**

Mark CS2 done. Record, per item:

- **CS2.1** — the radix tree was dead code; deleted rather than
  repaired. State plainly that the source document's headline finding
  was real as written code but unreachable, and that `proc_table`'s
  bucketed hash was always the live lookup path. This is worth writing
  down precisely because a future reader will otherwise re-derive the
  same alarm from the same comment.
- **CS2.2** — fixed, and note the deliberate deviation: the dynamic
  array came forward from CS4 because the correctness fix was not
  separable from it. Say what CS4 still owns.
- **CS2.4 / CS2.6** — the results, including "found nothing" if that is
  what happened. A stress test that reproduces nothing is a real
  outcome when it has been shown capable of catching the bug (Task 3
  Step 3).
- **CS2.3 / CS2.7** — already delivered by CS0; cross-reference the
  commits.
- **CS2.5** — still deferred, still a `clone()` pre-condition.

- [ ] **Step 2: Update the ABI record**

`select()`'s behaviour moved toward Linux, so refresh the relevant
section of `docs/abi-compatibility.md` and make sure the remaining
`poll()` cap divergence appears there as well as in `docs/stdlib.md`.

- [ ] **Step 3: Final verification**

```bash
make clean-kernel && make test 2>&1 | tail -2
make clean-kernel && make DEBUG_HEAP=1 test 2>&1 | tail -2
tools/gauntlet.sh 15 3
GAUNTLET_MAKEFLAGS=DEBUG_HEAP=1 tools/gauntlet.sh 15 3
```

Expected: `PASS` twice and `PGAUNTLET PASSED: 15/15` twice.

- [ ] **Step 4: Commit**

```bash
git add docs
git commit -m "CS2 done; record what the named regressions turned up"
```

---

## Notes for the executor

- **Tasks are independent** and may be reordered. Task 1 is the
  smallest and is a deletion; Task 4 is the largest.
- **Two tasks may find real bugs** (Task 3 Step 2, Task 4 Step 3). If
  one does, that is the milestone working. Report it with evidence and
  fix it in place — unlike CS1, a bug found here belongs to CS2, which
  is the regression milestone.
- **`serial_write_hex64` is the only kernel number formatter**; there is
  no `%d`. Userland tests have musl's `printf`.
- **New userland binaries need three Makefile edits**, not one: the
  build rule, `REQUIRED_MARKERS`, and the INITTAB spawn list. A binary
  that builds but is never spawned passes silently by never running.
