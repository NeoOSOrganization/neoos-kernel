# Concurrency hardening & scaling — design

**Date:** 2026-09-01
**Status:** design spec. Decomposed into CS0–CS5; each sub-milestone
gets its own implementation plan under `docs/superpowers/plans/` when
it is reached.

**Supersedes** the driver/audio half of
`docs/superpowers/specs/2026-08-31-post-smp-roadmap.md`. This is the
work that runs before BusyBox; x2APIC, FDC and the audio stack were
dropped to make room for it (see that file's "Dropped" section).

**Origin:** an external read of `main` at 281 commits, revised once
against the same tree. Every specific claim below was re-verified
against the working tree on 2026-09-01 before this spec was written;
the verification results are recorded inline as **[verified]**.

---

## 1. Why this milestone, and why now

NeoOS already has unusually good bones for concurrency work: a
rank-checked spinlock discipline that panics on inversion instead of
deadlocking silently, an in-kernel selftest per subsystem, and `make
test`'s `REQUIRED_MARKERS` gate that fails the build if a suite quietly
stops reporting. **Everything here extends that infrastructure — none
of it replaces it.**

What it does not have is any way to make a rare interleaving happen on
purpose. Every SMP bug found so far was found by luck: the waitq
double-free surfaced once, from a `free_list` pointer of `0xfffffffc`,
under one specific timer/SMP interleaving that nothing reproduces. A
single green `make test` is not evidence about anything
timing-dependent.

Two forcing functions make this urgent rather than nice-to-have:

- **BusyBox is next.** `ash` forks per external command and musl's
  mallocng gets its heap from `mmap`. BB0 already had to fix `fork`
  losing the vma list — a bug the existing tests missed because they
  only touched pages the parent touched first. The remaining fixed
  caps (`SPAWN_MAX_ARGS 8`, `POLL_MAX_FDS 16`, the fd 0/1/2 allocator
  special case) are exactly the surfaces a shell lands on.
- **The lifetime work landed but was never generalized.** The
  SMP-lifetime-and-lock-detangle milestone installed
  `proc_get`/`proc_put`/`thread_get`/`thread_put` and deleted the inert
  `kernel/sync/rcu.c` **[verified: `kernel/sync/` now holds only
  `lock.{c,h}`, `spinlock_types.h`, `waitq.{c,h}`]**. One place in the
  tree still does the unsafe "find in a table, drop the lock, use the
  pointer" pattern the plan's own spec calls out: `pid_alloc`'s radix
  tree.

## 2. Constraints (all sub-milestones)

- **No host unit tests.** Every test is a kernel selftest
  (`serial_write_string`) or a userland binary ending in a
  `[name] ALL PASSED` marker wired into `REQUIRED_MARKERS`. There is no
  host runtime to run tests in and none should be added.
- **The parallel gauntlet is the bar**, not `make test`. Each task
  closes on `PGAUNTLET PASSED: 15/15`
  (`tools/gauntlet.sh`,
  CONC=3). Three baseline flakes are known and tracked; a new
  signature is a regression.
- **Lock ranks are enforced.** Any new lock gets a `LOCK_RANK_*` slot
  in `kernel/sync/lock.h` with a written rationale. 27 ranks are
  defined today **[verified]**.
- **The ABI is not ours.** Anything a user program can observe — the
  `poll`/`select` fd caps' error behaviour, `execve` argv limits,
  POSIX lowest-available-fd allocation — matches Linux x86-64
  semantics. Every deliberate divergence goes in `docs/stdlib.md`.
- **Every user-facing change needs a musl path or a `lib/` wrapper**
  plus a `docs/stdlib.md` entry (CLAUDE.md).
- **QEMU is the reference machine:** `-cpu Nehalem -smp 4`.

## 3. Sub-milestone decomposition

```
CS0  Audit & confirm          ← reads only; resolves one open question
 └─ CS1  Instrumentation      ← build-once; everything after depends on it
     └─ CS2  Named regressions
         └─ CS3  Stress matrix + chaos
             ├─ CS4  Fixed limits
             └─ CS5  Locking architecture
```

CS4 and CS5 are independent of each other. Within CS5, the poll-table
redesign is gated on CS4's `poll`/`select` fix (both rewrite the same
loop, and the new registration path should not be designed around the
old 16-fd array shape).

**Deviation from the source document's sequencing, deliberate:** it put
the named regressions first and instrumentation third. This spec swaps
them, because that same document observes that the two hardest items
(the lockless `pid_lookup` and the VT-switch question) "depend on
poisoning to be debuggable rather than just detectable." Without the
poisoned heap, those tests produce corruption you can see but cannot
localize.

---

## CS0 — Audit and confirm

Two reads, no new subsystems. Both resolve questions that change what
later sub-milestones have to do.

### CS0.1 — Do the kernel VTs have any locking at all? — **RESOLVED: no, and it was a real bug**

**Answer: confirmed, fixed in commit `e1868ec`.** The audit looked in
the wrong file first. `console.c`, `con_driver.c` and `kvt.c` do contain
no locking — but that is by design for `kvt.c` ("PURE logic -- no
rendering, no locks"), and the state that actually needs guarding lives
in **`kernel/tty/vt.c`**, which the original document never examined.
`vt.c` had no lock either.

Three races, all reachable:

- **`vt_switch`** wrote the global `vt_active` and ran `render_full`
  over a VT's diff cache holding nothing, concurrently with a writer on
  another CPU inside `render_diff` on the same cache under `t->lock`.
  The consequence is not a transient tear: `render_diff` marks cells
  clean in `shown[]` as it paints them, so cells painted onto a screen
  a concurrent switch has just cleared are **never repainted**.
- **`vt_scroll`** mutated the active VT's `struct kvt` holding nothing,
  racing `kvt_feed` under `t->lock`.
- **`vt_active`** was a plain `int` read by six functions with no
  barrier.

Both unlocked paths are reached from the keyboard IRQ
(`input.c:97,106`) and — usefully — *before* `input.lock` is taken, so
they hold nothing, which is what made the fix tractable.

What was **not** broken: per-VT writes were already serialized, because
`tty_obj_write` holds `t->lock` across `vt_backend_output`.

**The fix:** `vt_lock` at `LOCK_RANK_VT` (251), guarding `vt_active`,
`shown`/`shown_valid` and `kd_mode`. It sits above `LOCK_RANK_TTY` and
below `LOCK_RANK_FBCON`, so the write path (`t->lock` → render → fbcon)
is ascending; `vt_switch` and `vt_scroll` take `t->lock` then `vt_lock`
from the IRQ in the same order. `waitq_wake_all` moved outside both,
since WAITQ ranks below VT.

**Evidence, not assertion.** `vt_stress_selftest` (in `vt.c`, run after
`smp_start_aps()` since it needs a second core) flips VTs on CPU 1
against a writer on the boot CPU and counts every `render_diff` that
paints a VT which is not the active one — impossible under the lock,
and guaranteed by every call site otherwise. It fired on 13/13 unlocked
runs and passes 20/20 locked, with gauntlet 15/15.

**A trap worth recording for CS1 and later.** The first version of the
fix latched `vt_panicking` inside `vt_panic_reset`. But `vt_selftest`
calls that reset in ordinary context, so a *selftest* silently disabled
VT locking for the entire rest of the boot — and the detector correctly
reported the unlocked renders that followed, which read at first like a
broken test. The latch now lives in `vt_enter_panic()`, called only
from the exception handler, exactly as `fbcon_enter_panic()` is. **A
one-way "we are panicking" flag must never be set by a function a test
can call.**

### CS0.2 — `spin_lock_raw` bypasses the rank checker in five files

The rank checker cannot see a lock taken with `spin_lock_raw`, whether
or not that lock has a registered rank. A registered-but-always-raw
lock is exactly as invisible as an unregistered one.

**[verified]** — every raw call site in the tree today:

| File | Lock | `spin_init`'d? | Every site raw? |
|---|---|---|---|
| `kernel/tty/pty.c` | `pool_lock` | yes, `LOCK_RANK_PTY` (`pty.c:260`) | yes |
| `kernel/fs/devfs.c` | `dyn_lock` | yes, `LOCK_RANK_DEVFS` | yes |
| `kernel/drivers/video/fbcon.c` | `fbcon_lock` | yes, `LOCK_RANK_FBCON` | yes |
| `kernel/drivers/char/serial.c` | `serial_lock` | yes, `LOCK_RANK_SERIAL` | yes — **documented**: pre-`cpu_local_init()` output |
| `kernel/lib/rand.c` | `rand_lock` | yes, but under **`LOCK_RANK_SERIAL`** (`rand.c:41`) | yes |

Note this corrects a claim worth not repeating: `pty.c`'s `pool_lock`
**is** properly `spin_init`'d. The bug is at the call sites, so the fix
is a `spin_lock_raw`→`spin_lock` swap, not a missing `spin_init`.

`rand_lock` sharing `LOCK_RANK_SERIAL` with the unrelated serial lock
is a separate finding. It is not the deliberate, commented
`LOCK_RANK_TTY == LOCK_RANK_DRIVER` pair `lock.h` already carries — it
reads as a copy of the nearest example rank, and it would hide a real
serial/rand inversion precisely because the checker has been told they
are the same rank on purpose.

**Action:** for each of `pty.c`, `devfs.c`, `fbcon.c`, `rand.c`,
confirm no call site runs before `cpu_local_init()`, then swap raw →
checked at every site. Give `rand_lock` its own rank, or justify the
sharing with a comment in the `TTY`/`DRIVER` style. Serial keeps its
documented exception.

**RESOLVED** (commits `7ff4323`, `511a4cb`, `efcdce2`, `76b2e39`).
Two findings worth keeping:

- **None of the four had the before-`cpu_local_init()` excuse.**
  `con_driver_select()` runs at `kernel.c:111`, after
  `cpu_local_init_bsp()` at line 93; `pty_init()` (141) and
  `rand_init()` (147) are later still. Only `serial.c` genuinely
  predates per-CPU state.
- **`fbcon` needed a raw path anyway, for a different reason.**
  `vt_panic_reset()` runs from `exception_dump_and_halt`, and the
  panicking CPU may hold any lock, so a checked acquire there would
  call `lock_panic()` from inside a panic and recurse. `fbcon_acquire`
  picks raw or checked from the `fbcon_enter_panic()` latch; `vt.c`
  skips its lock entirely under the same condition. Verified by hand
  with a deliberate page fault, since `make test` never faults: the
  dump paints with no rank-inversion panic.

`rand_lock` moved to its own `LOCK_RANK_RAND` (250).

---

## CS1 — Instrumentation to build once, use everywhere — **DONE**

Landed 2026-09-01 (`eb41951`, `20d3fe3`, `ec0e577`, `dd91b12`,
`3011f49`, `c6ced21`). Full usage in `docs/debugging-tools.md`.

- **Poisoned-free / redzone heap mode** — `make DEBUG_HEAP=1`. Freed
  size-class slots are filled with `0xDF`; every slot handed out is
  verified still poison; a free magic plus a free-list walk catches
  double frees; and `[requested, size_class)` is filled with `0xBB` and
  checked on free. Each of the three detectors was proved by making it
  fire, then reverting the proof.
  Two constraints shaped this and are worth not rediscovering: `kfree`
  gets no size (only `page->size_class`), so the requested length lives
  in a `req[]` table in the **page header** — a per-allocation header
  in front of the slot would shift every slot off the 64-byte alignment
  `fxsave`/`XSAVE` buffers in heap objects depend on, which `heap.c`
  already has a `#GP` in `schedule()` on record for.
  **Known limitation, deliberate:** large allocations are not covered —
  their memory returns to the pmm on free, leaving nothing to check.
- **Lock hold-time / contention histogram** — `make DEBUG_LOCKSTAT=1`,
  dumped at shutdown as rank / count / max_tsc / long_holds. Per-CPU
  and lock-free by construction; the row is found in O(1) from an index
  cached in the CPU block. Sparse ranks fold into 38 slots × 16 buckets
  (~5.5 KiB/CPU) because the naive table would have been ~9 MiB of
  `.bss`.
  **First measurement, and a CS5 target:** rank 8
  (`LOCK_RANK_TTY`/`DRIVER`) puts **2209 of 2260 holds in the top
  bucket** (max ~118M TSC ticks) — the console render path holding a
  tty lock across a full framebuffer paint.
- **Promote the gauntlet.** It was worse than "buried in a milestone
  directory": it was **untracked**, matched by
  `.superpowers/sdd/.gitignore`, so nothing preserved it and no plan
  could point at it. Now `tools/gauntlet.sh`, working in
  `build/gauntlet`, with a `GAUNTLET_MAKEFLAGS` passthrough so CS2/CS3
  can stress the poisoned kernel, and a per-marker flakiness table so a
  marker missing 1 run in 15 reads as `6% 1/15 …` instead of
  disappearing behind a green `15/15`.

**Baseline for CS2:** 15 boots under `GAUNTLET_MAKEFLAGS=DEBUG_HEAP=1`
produced **zero heap panics**. The current kernel has no use-after-free,
double free, or size-class overrun reachable by a full boot of every
suite. That is the clean starting point CS2's regression tests are
written against — and it means any heap panic CS2/CS3 produces is new
information, not pre-existing noise.

## CS2 — Named regressions — **DONE**

Landed 2026-09-01 (`5990938`, `1a2c0b6`, `da08e46`, `463514c`,
`fc7b0e4`). Four items needed work; three did not.

**CS2.1 — the lockless `pid_lookup` was dead code.** This was the source
document's headline finding, and it dissolved on inspection. The race is
real as written — `pid_lookup_internal` walks shared radix nodes with no
lock, `pid_insert_internal` publishes them with plain stores — but it is
**unreachable**: `pid_insert`, `pid_remove` and `pid_lookup` had no
callers anywhere in the tree, so `root` was permanently `NULL` and every
lookup returned on its first line. Real process lookup is
`proc_table_lookup`'s bucketed hash: per-bucket locks, refcounted
results since the SMP-lifetime milestone. **Deleted rather than
repaired**, along with `pid_alloc_specific` and its sole caller
`proc_table_alloc_pid_zero`, which could never have worked — it passed
`0` to a function whose first line rejects `pid <= 0`.
*Worth recording because a future reader will otherwise re-derive the
same alarm from the same comment.*

**CS2.2 — `select()` silently dropped ready fds.** Reproduced exactly:
`select reported 16 of 20 ready`. It now counts the caller's set bits,
allocates for that many, and reports all of them.
**Deviation from this spec, deliberate:** the dynamic array came forward
from CS4, because the correctness fix is not separable from it — the bug
*is* the fixed array, and the alternatives without removing it are
"silently drop" (the bug) or `-EINVAL`, which breaks a shell just as
badly and is not Linux's behaviour. **CS4 still owns** `poll()`'s own
`POLL_MAX_FDS` cap and the `FD_TABLE_MAX` bound.

**CS2.4 — the waitq exit-vs-drain race now has a regression test**, and
proving it discriminating took two attempts. A deterministic double
`thread_put` in the drain crashes with *or* without the churn, since
ordinary selftest threads already exercise that path. Injecting the
**real** race instead — removing `thread_wait_off_cpu` — gives the
result that matters:

| | outcome |
|---|---|
| race reintroduced, churn **off** | clean boot, zero panics — invisible |
| race reintroduced, churn **on** | `[heap] PANIC: use-after-free write` |

That is CS1's poisoned heap doing precisely the job it was built for.

**CS2.6 — the TLB shootdown holds up under contention.** 4 processes ×
150 rounds of mmap/touch/mprotect/munmap: the timeout path never fires,
and no frames are lost. `TESTHOOK_PMM_FREE` exposes the free-frame count
to userland.
**One honest limit**, found by trying to prove the frame assertion
fires: every leak big enough to cross its threshold kills something
first — disabling the deferred `pmm_free` starves the machine before the
test runs, and leaking one frame in four gets the test process killed
mid-storm. The count is a reported number and a second line of defence;
the child-status checks are what actually catch a broken deferred free.

**CS2.3 and CS2.7 were already delivered by CS0** — the adversarial
rank-checker selftest (`6b12e35`) and the VT-switch locking fix
(`e1868ec`).

**CS2.5 remains deferred.** `clone()` still does not exist, so
`fd_table_dup2`'s race is unreachable; it stays a blocking pre-condition
recorded against that milestone.

**A CS0 defect found during CS2 and fixed here:** `vt_stress_selftest`'s
call site in `kernel.c` was never committed. CS0 Task 5 added it, then
reverted `kernel.c` to remove a temporary fault injection and took the
call with it — so the VT stress test had not run since. Both stress
selftests are now wired, and placed **before** `spawn("/SBIN/INIT.ELF")`
rather than after: run alongside the userland workload, `vt_stress`
cleared the screen under TERM and failed its render check.

**Also swept, at the user's request** (`1a2c0b6`): 23 dead functions
from earlier kernel eras, found with linker garbage collection rather
than grep — grep cannot see vtable or assembly callers and wrongly
flagged `vgacon_putc`, `fbcon_clear` and `isr_handler`, all live. Mostly
the streaming character-at-a-time console API from before M1b/M1c, plus
`thread_table_alloc_tid`/`thread_table_lookup` superseded by refcounted
lookups.

## CS3 — Stress matrix and chaos harness

Reusable userland binaries under `userland/`, each with its own
`[xxxtest] ALL PASSED` marker in `REQUIRED_MARKERS`:

| Binary | Targets | Method |
|---|---|---|
| `forkstorm` | `pid_alloc`, `proc_table`, `fd_table`, `thread_table` | N processes forking+execing tightly across all CPUs for a fixed window; verify PID reuse never collides a live PID (`wait4` every child) and `fd_table_count` returns to 0 after each exit |
| `mmapracer` | `vma.c`, `tlb.c`, paging | Threads sharing one address space (kernel threads until `clone()`) hammering `mmap`/`mprotect`/`munmap`/faults on overlapping and adjacent ranges; no `SIGSEGV` on addresses that should be valid, no corruption of a canary written after each fault |
| `faultflood` | demand paging, `pmm_alloc` under pressure | Touch far more distinct pages than physical memory allows, across CPUs at once, forcing `vma_fault_locked`'s allocation-failure paths under real contention rather than single-threaded OOM |
| `pollstorm` | `waitq.c` poll broadcast, `sys_poll.c` | M threads polling disjoint fd sets while a driver thread pushes unrelated readiness at rate R; record wakeups vs. events to **quantify** the O(M) thundering herd, giving CS5.2 a baseline |
| `ptychurn` | PTY pool, the now-checked `pty.c` locks | Open/close ptys crossing `PTY_MAX` repeatedly from multiple CPUs, racing master/slave open+close against read/write to pressure `pty_unref`'s `gone` check |
| `sigstorm` | signal delivery atop the landed lifetime work | `kill`/`tkill` storms overlapping process/thread exit from other CPUs — a regression suite for already-landed refcounting, not a test blocked on future work |
| `rankinvert` | the rank checker itself | Deliberately-wrong-order acquisitions across every defined rank pair (27 today), as a kernel selftest that expects `lock_panic` and treats *not* panicking as the failure |

Chaos knobs, all debug-build only:

- **`NEOOS_DEBUG_HZ`** — raise the LAPIC timer frequency so preemption
  windows that are rare at the normal tick become common. Run the full
  suite at both rates; CS2.4 and CS2.6 are exactly the shape of bug
  that only appears when the window widens.
- **`pmm_alloc` failure injection** — deterministic Nth-allocation
  failure, plus seeded-random failure so a hit is reproducible from the
  seed printed to serial. Every `if (!frame)` / `-ENOMEM` path in
  `vma.c`, `tlb.c` and `fd_table.c` should be hit at least once per
  run.
- **Concurrent teardown injection** — `SIGKILL` from another CPU at a
  randomized point relative to the target's own syscall progress
  (a `PAUSE` loop of random small count at a designated fuzz point in a
  debug build). This is what actually exercises the refcounting; a
  scripted single-shot kill does not.

---

## CS4 — Fixed-size limits

For each: identify the real workload that sets the size, make the
smallest change that removes the ceiling, and add a test that exceeds
the *old* limit.

| Limit | File | Current | Direction | Test |
|---|---|---|---|---|
| PTY pool | `kernel/tty/pty.c` | `PTY_MAX 16`, flat array | Growable structure under the now-checked `pool_lock` (CS0.2) | `ptychurn` crosses the old ceiling; the 17th open no longer returns `-ENFILE` |
| `spawn` argv | `kernel/sched/proc.h`, `sys_proc.c` | `SPAWN_MAX_ARGS 8`, `SPAWN_ARG_MAX 128`, fixed `argv[8][128]` | Copy argv/envp the way Linux does: walk the user pointer array for `argc` and total bytes, allocate for the real total, copy once — capped by a generous overall byte budget (`ARG_MAX`-style), not a fixed slot count | `execve` with a realistic compiler-style invocation (dozens of args, long paths); assert it runs rather than truncating |
| fd 0/1/2 reservation | `kernel/sched/fd_table.c` | `fd_table_alloc` special-cases `bucket_idx == 0` with `first = FD_STDIO_COUNT`, so a closed fd 0/1/2 is never reissued — non-POSIX | Remove the special case entirely. `fd_table_put` already sets 0/1/2 at process creation; once closed, the normal first-free scan should find them like any other slot, and the dup2 workaround becomes unnecessary | Close fd 1, `open()`, assert the new fd *is* 1 |
| `poll`/`select` caps | `kernel/syscall/sys_poll.c` | See CS2.2 | Fix the correctness bug first, then replace the fixed `pfd[POLL_MAX_FDS]` stack array with one sized to the caller's actual `nfds`, capped by `FD_TABLE_MAX` so an untrusted argument cannot drive an unbounded kernel allocation | `poll()`/`select()` with 100+ fds all ready; assert all reported |
| `MAX_THREADS_PER_PROC 16` | `sched/thread_table.h`, `proc.h`, `proc.c` | Fixed cap, and fixed `victims[]` snapshot arrays; the header's own comment says "Phase 2 will increase" | Convert `thread_table` to the bucketed-dynamic pattern `fd_table` already uses; make the kill/exit `victims[]` snapshots dynamically sized or linked. Not optional once `clone()` lands — musl's pthreads needs it | More threads than the old cap via a kernel-thread stand-in today; real pthreads once `clone()` lands |
| PID wraparound | `sched/pid_alloc.*` | `pid_alloc` returns 0 once `next_pid >= MAX_PIDS` and never reuses below that; the comment admits *"PID space exhausted; would need wraparound + collision handling"* | Linux-style cyclic allocation: wrap to a low-water mark and scan for the first PID absent from the radix tree, rather than relying only on the free list (which covers only PIDs freed in order). **CS2.1 is a prerequisite** — wraparound makes lookup-during-reuse far more frequent | Exhaust the space with an artificially small `MAX_PIDS`; confirm allocation continues via wraparound and no wrapped PID is issued while still live |
| `kvt.c` `params[8]`, `TTY_BUF`, evdev ring | `tty/kvt.h`, `tty/tty.h`, `drivers/input/evdev.c` | Fixed, sized for interactive single-user use | Lowest priority. An escape sequence with >8 params or a TTY line over 1KB is a pathological input, not a compiler invocation — size generously (Linux `N_TTY` ring conventions) rather than making these dynamic; dynamic growth here buys complexity for a rare case | Over-length canonical line and an over-param escape sequence; assert graceful **documented** truncation, not a buffer overrun |

`MAX_CPUS` is **already 128** **[verified: `kernel/arch/cpu_local.h:10`]**
— the constant needs no change. What remains is validation only:
confirm the arrays sized by it in `smp.c`/`tlb.c` behave above 4 real
cores under QEMU `-smp`, since raising the ceiling does not by itself
prove nothing still assumes ≤4 internally. Folded into CS3's scale
runs; not a code change.

---

## CS5 — Locking architecture

Sequenced changes, several depending on CS4.

**CS5.1 — the global `vfs_lock`.** Do not rewrite in one pass. The rank
list already reserves `LOCK_RANK_MOUNTTABLE` (4), `LOCK_RANK_VNODEHASH`
(5) and `LOCK_RANK_VNODE` (6) — the granularity wanted already has slots.
Audit what `vfs_lock` protects that a vnode's own lock or the mount
table's does not, path by path (`lookup`, `mount`/`umount`,
`getdents64`), and peel each off individually, keeping `vfs_lock` as a
shrinking fallback until nothing calls it. Measure with a
`forkstorm`-shaped concurrent `open`/`stat`/`getdents64` test across
same and different mount points, before and after each peel.

**CS5.2 — `poll_broadcast`'s thundering herd.** The biggest
architectural win available, and already diagnosed in `sys_poll.c`'s own
header comment. Once `pollstorm` has a baseline wakeups-per-event
number, replace the single global broadcast waitq with a real poll
table: each pollable object (`file_ops.poll` already exists as the
readiness check) grows its own small waitq that `poll_core` registers on
only for the fds it was asked about — Linux's `poll_wait()`/`epoll`
split rather than waking everyone on every event. **After CS4's fd-cap
fix**, since both rewrite the same loop.

**CS5.3 — the hand-maintained rank enum stays.** At 27 ranks this is
fine and arguably a feature: the per-rank comments in `lock.h` are some
of the best documentation in the codebase, and real lockdep would lose
them. Two cheaper wins instead of a rewrite:
a CI-time script (host-side, parsing `lock.h` — not a kernel selftest)
that fails when two `LOCK_RANK_*` values are equal without a paired
comment justifying it, catching the "renumbering forgot one" mistake at
commit time instead of at boot-panic time; and revisiting a dynamic
registration scheme only if the count actually approaches the 100+
scale that would motivate it. Build the script first — CS0.2's
`rand_lock`/`LOCK_RANK_SERIAL` collision is its first real finding, and
is exactly what it would have caught on day one.

**CS5.4 — `spin_lock_raw` escape hatches.** CS0.2 does the cleanup;
CS5.4 adds the CI grep that fails review on any new `spin_lock_raw`
outside the documented exceptions, the same way `REQUIRED_MARKERS`
fails a build that silently drops a suite.

**CS5.5 — refcounting and seqlocks.** RCU is gone and refcounting is
live: `proc_get`/`proc_put`, `thread_get`/`thread_put` and the
streaming ref'd `proc_table` iterator are in `kernel/sched/proc.c`
today **[verified]**. So this is not "build a mechanism" — it is
"apply the existing one to the last place that needs it," namely
`pid_alloc`'s radix tree (CS2.1). Do not design a second RCU-like
mechanism alongside it.
Seqlocks are then worth introducing for the physmap and `proc_table`
iteration paths that serialize on refs-plus-copy today: with the
refcounting groundwork already committed, a seqlock-protected iterator
(optimistic read, retry on generation mismatch) is a much smaller diff
than it would have been, and gives read-mostly table scans — a
`ps`-equivalent, `/proc`-style enumeration — with no lock at all in the
common case.

---

## 4. What this milestone does not do

- **No `clone()`.** CS2.5 and CS4's thread-cap row are written as
  pre-conditions recorded against that milestone, not work done here.
- **No new device drivers.** x2APIC, FDC and audio were dropped from
  the roadmap in favour of this work.
- **No lockdep.** CS5.3 explicitly declines to build it.
- **No NUMA.** Out of scope, as it was for the SMP milestone.

## 5. Milestone close

Refresh `docs/abi-compatibility.md` (the `poll`/`select`, `execve`
argv and fd-allocation semantics all move toward Linux here), update
`docs/stdlib.md` with any new divergence, and record the measured
before/after numbers CS5.1 and CS5.2 produce — both are throughput
claims and should be backed by the histograms CS1 builds.
