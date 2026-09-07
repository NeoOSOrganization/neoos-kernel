# Concurrent Request Crash — Investigation Plan

## Status

ASP.NET Core / Kestrel serves HTTP on NeoOS (`docs/aspnet-missing-
syscalls.md`): **50/50 sequential requests**, correct `200 OK` +
body. Under **3+ concurrent in-flight requests** the .NET process
dies — either a hang or a hard `AccessViolation`. This spec is the
plan to root-cause it. It is a prerequisite check-item for the SMP
load balancer (SCH-2 in the advanced-scheduler spec): the balancer
must not be built on top of a broken SMP invariant.

## What is known (do not re-derive)

Faulting instruction, consistent across runs:

```
[usrflt] vec=0x0e err=0x06 cr2=0x5000042e8008 rip=0x20000036df2e
```

- `vec 0x0e` = page fault, `err 0x06` = **write, user-mode, page
  NOT present** (`P=0, W=1, U=1`). Not a protection fault — the page
  is genuinely unmapped.
- `cr2` in the `0x5000_0000_0000` range = the process's mmap/heap
  region. Different every run, always `…008` or a page boundary.
- `rip` resolves (via `WebTest.dbg` `nm`) to
  **`ConcurrentQueueSegment<SocketAsyncEngine.SocketIOEvent>::.ctor
  + 0x4e`** — the loop `for (i…) _slots[i].SequenceNumber = i;`
  right after `_slots = new Slot[boundedLength]` (boundedLength = 32
  initially). Disassembly: `mov %eax, 0x10(%rdx,%rdi,8)` with a
  preceding `lea (%rdi,%rdi,2)` → 24-byte `Slot`, `+0x10` =
  `SequenceNumber`.
- So: **`new Slot[32]` returned a pointer into unmapped memory**, or
  the array object header was overwritten so its element base /
  length is garbage. Either way this is **GC-heap corruption** — the
  fault is just where the corruption first gets dereferenced.
- Trigger: several .NET threadpool threads handling requests in
  parallel, each doing socket recv/send + `epoll_ctl` + close, while
  the GC and the `SocketAsyncEngine` event-loop thread also run.
  This is the most concurrent multithreaded single-process workload
  NeoOS has ever executed.
- **Not** triggered by: sequential requests (1 thread active at a
  time), the raw-`TcpListener` server (blocking accept, 1 thread),
  any prior workload (BusyBox, coreutils, the .NET hello/tcp/thread/
  gc tests — none put this many threads on shared memory).
- Adding kernel serial-print instrumentation to the hot path makes
  the crash **go away** (timing-sensitive → a race, not a logic
  bug).

## Phase 1 progress (2026-09-07, session e4910de7)

**Done:**
- `userland/mmstress.c` + `make mmstress` (`SMP_CPUS=` overridable) — the
  .NET-free oracle: 8 threads of one process doing mmap/munmap/mprotect
  + fault + pattern-verify, own boot.
- Non-perturbing fault diagnostics landed: `isr.c` `[usrflt]` line
  (cpu/tid/vec/err/cr2/rip, rate-limited) and `signal.c` `[fault-audit]`
  (PTE-vs-VMA classification at fatal user SIGSEGV).
- **Two real bugs found and fixed** (commit `e678d45`):
  1. `thread_create()` entered a SysV C function (libneoos
     `thread_trampoline`) with a page-aligned user RSP; the ABI wants
     `rsp%16==8` at a function entry, so the first stack `movaps` #GP'd.
     Fixed: plant `user_stack_top - 8`. Explains why **NeoOS-native**
     multithreaded programs crashed but musl/.NET (`clone_task`,
     caller-aligned `child_stack`) did not.
  2. `arch_prctl(ARCH_SET_FS)` did not update `schedule()`'s per-CPU
     `fs_base_loaded` cache — the next thread whose `fs_base` matched
     the stale cache would run with the previous thread's TLS base
     (investigation suspect #3). Fixed + corrected the false comment
     in `sched.c`.

**Negative result (informative):** with both bugs fixed, `mmstress` is
**clean at `-smp 1/2/4`** for the pure mmap/munmap/mprotect/fault path.
The core per-process mm ops (all under `p->mm_lock`, TLB settled after
release, frames deferred until shootdown ACK) **are SMP-safe under many
threads of one process.** The audit of suspect #2 (TLB shootdown)
found the ordering correct — every `context_switch` writes CR3
unconditionally, so a CPU only holds a live user-TLB entry for P while
actively running a thread of P, which is exactly what the shootdown
scan targets. The one residual risk there is the 50M-spin timeout
force-zeroing `shootdown_pending` and then freeing frames.

**Still open:** the .NET concurrent-request `AccessViolation` is NOT
reproduced by the current oracle. Next per Phase 1a's "escalate"
branch: add concurrent `epoll_ctl`/`close` churn, `brk`, and a brief
`fork`-COW to `mmstress`, and bisect which addition breaks it. Do NOT
do an mm-locking redesign without the user.

## Phase 1a "escalate" progress (2026-09-08, session b89806d7)

**ROOT CAUSE FOUND — and it is not SMP and not mm-locking.**

`mmstress` was escalated (`userland/mmstress.c`: raw-syscall epoll/pipe/
close churn, `brk` churn, `fork`-COW phase, all behind `MMS_*` compile
knobs). But the pure-mm path — every `MMS_*` off, i.e. the exact code
Phase 1 declared "clean at -smp 1/2/4" — **fails deterministically at
-smp 1** as well as 2 and 4. The Phase 1 "clean" claim was wrong (or
the libneoos rebuild since changed the pthread/alloc profile enough to
expose it). Signature: `[mmstress] FAIL mmap` (a 1-page `mmap` returns
`MAP_FAILED`) and/or the `[usrflt] … cr2=…fff` + `[fault-audit] VMA=
covered … no PTE` fault — **the identical signature as the .NET crash**
— followed by a flood of `[tlb] out of memory deferring a frame;
leaking it` and pmm draining from 0x74f2 free frames to ~0x60.

Instrumentation added to `kernel/smp/tlb.c` (`tlb_dbg_*` counters, dumped
by `tlb_dbg_dump()` from `kernel_shutdown`) and a per-exit line in
`proc_put_live`. A representative -smp 1 run:

```
[tlb-dbg] exit pid=2 pml4=0x00fd7000 deferred+=0x810   (2064 frames)
[tlb-dbg] exit pid=3 pml4=0x0600c000 deferred+=0x810
[tlb-dbg] exit pid=4 pml4=0x06826000 deferred+=0x810
[tlb-dbg] exit pid=5 pml4=0x01058000 deferred+=0x810
[tlb-dbg] exit pid=6 pml4=0x01861000 deferred+=0x810
[tlb-dbg] exit pid=7 pml4=0x0407f000 deferred+=0x810
[tlb-dbg] deferred=… freed=… leaked=0x44d flush=…
          skipowner=0xe1201 queued_now=0x100 overflow_now=0x2f60
```

`queued_now (256) + overflow_now (12128) = 12384 = 6 × 2064` — the
entire address space of every one of pids 2–7.

**The mechanism:**
1. The TLB deferred-free queue (`kernel/smp/tlb.c`) is drained by
   exactly one thing: a `tlb_shootdown(owner_pml4)` whose argument
   matches the `owner` tag the frames were deferred with. `vma_*`'s
   `vma_tlb_settle` does this for live munmap/mprotect; `proc_reap`
   does it (`tlb_shootdown(p->reap_pml4_phys)`) for a process's whole
   address space, which `proc_put_live` / `process_exit` only *defer*
   (they run with IF off on a ZOMBIE thread and cannot shoot down).
2. **`proc_reap` only runs when a zombie is `wait()`ed.** Pids 2–7 are
   the boot-time network self-tests (arp/icmp/tcp/dhcp/…). They exit,
   are reparented to init — and the `mmstress` INITTAB is just
   `wait /mmstress.nex`, so init never reaps them. Their 12 384
   deferred frames sit in the queue **forever**.
3. Those orphans saturate the 256-entry `deferred_frames[]` fast array
   (all with an `owner` no future shootdown targets — hence
   `skipowner` in the hundreds of thousands: every `mmstress` flush
   re-walks and re-skips all 256). Every subsequent defer — including
   all of `mmstress`'s own munmap frames — detours onto the unbounded
   `deferred_overflow` kmalloc list.
4. `mmstress`'s own overflow frames *do* drain on its own flushes, but
   the constant kmalloc/kfree churn plus the permanently-pinned 12 384
   frames drives pmm to exhaustion; then `kmalloc` for an overflow
   node fails and `tlb_defer_free` **leaks** the frame outright
   (`leaked=0x44d`). Past that point `vma_fault` can't get a frame →
   PTE never installed → "VMA covered, no PTE" → SIGSEGV.

**Why .NET shows it as a rare concurrent-only crash:** same exhaustion,
reached more slowly. A long-lived .NET process doing GC `mmap`/`munmap`
+ thread-stack churn on a system that already has thousands of orphaned
boot-selftest frames pinned just needs enough munmap volume to cross
the threshold; more concurrency = more munmap/s = crosses it during a
request instead of never. "Adding serial prints makes it disappear" =
slower = fewer munmaps before the workload finishes.

**Fix options (for the user to choose):**
- **A. Opportunistic drain of dead-owner frames.** When `free_address_
  space` defers a whole address space and every thread of the process
  is already off every CPU (true at `process_exit`: the exiting CPU
  has already reloaded CR3 to `p4_table`, and `schedule()` writes CR3
  on every switch so no other CPU can still hold that dead pml4), the
  frames are immediately safe to `pmm_free` with **no IPI at all**.
  Drain them right there instead of waiting for a `proc_reap` that may
  never come.
- **B. Saturation safety-valve.** When `deferred_n` hits `DEFER_MAX`,
  do one full `tlb_shootdown(0)` (always safe, drains every owner)
  before falling back to the overflow list. Cheap; bounds the damage
  from any orphan source, not just unreaped zombies.
- **C. Reap orphans.** Kernel-side reaper for processes reparented to
  init, or make init `wait()`-loop. Addresses the zombie leak only;
  A or B are more robust.

### Fix landed

**B implemented** in `kernel/smp/tlb.c:tlb_shootdown`: when the deferred
queue has a backlog (overflow list non-empty, or the 256-entry fast
array full), the shootdown is promoted to a full one (`pml4_phys = 0`)
before it runs — a full shootdown reaches every CPU and releases every
owner's frames, so it clears the orphans. Self-limiting: the first
promoted shootdown drains the backlog, the next caller is not promoted.
Not an mm-locking change; no new lock order.

Verified with `userland/mmstress.c` (pure mm, all `MMS_*` off):
- **before:** deterministic fail at -smp 1/2/4; pmm drains 0x74f2 → ~0x60.
- **after:** `[mmstress] ALL PASSED` reliably at -smp 1 and -smp 4;
  pmm healthy at shutdown (~0x6780 free); `[tlb] out of memory` flood
  gone; `deferred == freed`, `leaked == 0`.

A (drain dead-owner frames at exit without waiting for `proc_reap`) not
implemented — B alone fixes the observed failure and is the safer
minimal change. A stays on the table if an orphan source appears that B
can't keep up with.

### Secondary finding (not yet fixed) — -smp 2 thread-create EAGAIN

With the leak fixed, `mmstress` at **-smp 2** still intermittently hits
`[mmstress] FAIL pthread_create idx=7 rc=-11` (`-EAGAIN`) — the 8th
worker. `clone_task` returns 0 (→ `-EAGAIN`) from one of: `thread_alloc`
kmalloc failure, or `pmm_alloc(KERNEL_STACK_ORDER)` (an **order-2 /
16 KiB contiguous** block) failing. pmm has ~26 000 free frames at the
time, so the likely cause is **buddy-allocator fragmentation**: 8
threads hammering order-0 mmap/munmap leave no free order-2 block for a
kernel stack. Newly *visible* (the test now runs long enough to reach
that state), not newly caused. -smp 1 and -smp 4 are clean. Worth its
own investigation — a kernel-stack allocator that can't fall back to
non-contiguous pages, or a small reserve pool, is the likely fix.

### Escalation status (`userland/mmstress.c` `MMS_*` knobs)

- `MMS_EPOLL` (pipe + `epoll_ctl` ADD/DEL + `epoll_wait` + close churn
  on one shared epoll fd), `MMS_BRK` (inert — `sys_brk` is a stub),
  `MMS_FORK` (fork-COW phase). Each knob **alone** passes at -smp 4.
- `MMS_EPOLL + MMS_BRK` together fails at -smp 4 with the same
  frame-exhaustion signature — i.e. "epoll churn + one more thread"
  drives pmm down faster than the workers can recycle. Overlaps the
  -smp 2 EAGAIN finding; both point at allocator behaviour under this
  many threads, not at a correctness race. Not yet bisected further.

## Phase 1 — Root-cause investigation (no fixes yet)

### 1a. Reproduce deterministically, minimally, without .NET

Write a pure-C, musl-linked pthread stress test (`userland/`):

```
N threads (N = 2·nproc). Each loops:
  p = mmap(NULL, SZ, PROT_READ|PROT_WRITE, MAP_ANON|MAP_PRIVATE, -1, 0);
  touch every page of p (write a known pattern);
  verify the pattern;
  munmap(p, SZ);
Also: one thread doing malloc/free churn of varied sizes.
Also: threads that fork()? no — .NET doesn't fork. Keep it to
  clone()'d threads sharing one address space.
Run for a fixed number of iterations; CRC the pattern on every
verify; abort loudly on mismatch or SIGSEGV.
```

Vary: `SZ` (page, several pages, MB), thread count, whether pages are
pre-faulted vs demand-faulted, `MAP_POPULATE`, `madvise(DONTNEED)`
between iterations.

**If this crashes/corrupts → the bug is in `kernel/mm/` under SMP,
and .NET is just the messenger.** This is the expected outcome and
the fastest path.

**If it does NOT crash** → escalate the test: add `mprotect`
toggling, `mremap`, COW via a brief `fork`, concurrent `brk`, and
threads that `epoll_ctl`/`close` sockets in parallel (bringing in the
new epoll registry + `epoll_forget_fd` path). Bisect which addition
makes it fail.

### 1b. Bisect the concurrency

- Does the .NET webtest crash with QEMU `-smp 1`? `-smp 2`? `-smp 4`?
  If `-smp 1` is clean and `-smp 2` fails → a genuine SMP race
  (memory ordering, missing barrier, unlocked shared state). If even
  `-smp 1` fails → a re-entrancy / preemption bug (a thread
  preempted mid-critical-section on one CPU).
- Does it crash with the interrupt/preemption tick disabled for the
  test process? (a debug knob: `SCHED_FIFO`-pin the .NET threads
  once SCH-3 exists, or a temporary "no preempt" hack).
- KVM vs TCG: does `-enable-kvm -cpu host` change the failure rate?
  (TCG serialises more; KVM has true parallelism and weaker
  observed ordering — a bug that needs real reordering shows up
  more on KVM.)

### 1c. Instrument the boundaries (without perturbing timing)

Serial prints perturb timing (known — they hide the bug). Use
non-perturbing instrumentation instead:

- **Guard pages / redzones** around every `mmap` region and every
  kernel `kmalloc` (there is a `DEBUG_HEAP` build already —
  `GAUNTLET_MAKEFLAGS="DEBUG_HEAP=1"`; extend it to poison + check
  redzones on free, and to guard-page user mmap allocations). A
  redzone violation *names the culprit allocation* at the moment of
  the bad write, which `cr2` alone does not.
- **A ring buffer, not serial.** A lock-free per-CPU event ring
  (`{tsc, cpu, event, a, b}`) that the mm/sched/epoll hot paths
  append to, dumped to serial only on fault or on `neo_rt_status`-
  style request. Records the last N thousand
  `mmap/munmap/mprotect/fault/tlb-shootdown/context-switch/
  epoll_ctl/close` events with args — reconstruct the sequence that
  led to `cr2` after the fact.
- **PTE audit at fault time.** In the ring-3 `#PF` handler, before
  raising SIGSEGV: walk the page tables for `cr2` and log what's
  actually there (present bit, the VMA that should cover it, whether
  another CPU is mid-shootdown for that range). Distinguishes "VMA
  says mapped but PTE clear" (a lost/racy `paging_map_into` or a
  premature `munmap`) from "no VMA at all" (a genuinely wild
  pointer, i.e. the corruption is *inside* .NET's own bookkeeping).

### 1d. Read the suspects

Ranked by likelihood, each to be confirmed or eliminated:

1. **`kernel/mm/` address-space ops are not safe against
   concurrent threads of one process.** NeoOS's `mm_lock` (rank 3)
   — check that *every* path that reads or writes the VMA list or
   the page tables of the current process takes it, on *every* CPU:
   `sys_mmap`/`munmap`/`mprotect`/`mremap`/`brk`, `vma_fault`,
   `paging_handle_cow_fault`, the `copy_to_user`/`copy_from_user`
   fault fixup path (`isr.c` lines ~142–176), `elf_load`. .NET has
   ~10 threads all faulting and the GC calling `mprotect` /
   `madvise(MADV_DONTNEED)` on the heap — a `munmap` or `mprotect`
   on one CPU racing a fault on another, with insufficient locking,
   produces exactly "PTE cleared under a live pointer."
2. **TLB shootdown race.** `kernel/smp/tlb.c` — when CPU A unmaps a
   page, CPU B's TLB must be flushed *before* the frame is freed and
   reused. Check: is the frame returned to `pmm` only after all
   target CPUs have ACKed the shootdown? The `[tlb] out of memory
   deferring a frame; leaking it` lines seen in one crashed run
   suggest the deferred-free queue overflowed — under which
   condition does the code free eagerly instead? (`LOCK_RANK_TLB`
   is 24/25.) A frame freed before B's flush → B writes through a
   stale TLB entry into a page that's now something else → the
   "corruption" is a legitimate write to a wrongly-cached
   translation.
3. **`fs_base` / TLS under many threads.** `git log` shows two
   recent fixes here ("clone()'d threads never synced fs_base",
   "clone_task must not enqueue before sys_clone writes *ptid").
   The `schedule()` FS_BASE cache (`c->fs_base_loaded`) — is it
   correct when a thread migrates between CPUs *while another thread
   of the same process is also migrating*? A thread reading its
   `__thread` GC `alloc_context` through a stale FS base would
   allocate from another thread's context → two threads bump the
   same bump-pointer → overlapping allocations → the exact
   `new Slot[]`-returns-garbage symptom.
4. **The epoll changes from the ASP.NET bring-up.** `epoll_wait`'s
   re-snapshot loop, `epoll_forget_fd` from `fd_table_close`, the
   new `g_epoll_lock`. Under concurrent close+ctl+wait: is there a
   path where `epoll_forget_fd` (holding `g_epoll_lock` then
   `o->lock`) and `epoll_wait_core` (holding `o->lock`) or
   `epoll_ctl_do` deadlock or use-after-free? The re-snapshot loop
   `kmalloc`s `pfd`/`edata` every iteration — a `kmalloc`/`kfree`
   MT bug in the kernel heap would corrupt *kernel* memory, but if
   the heap and the process page tables share... check. Mitigation
   already in place: `epoll_wait` drops `POLLNVAL` entries so a
   just-closed fd's stale `data` isn't returned. Verify that's
   sufficient.
5. **Kernel heap (`kmalloc`) MT-unsafe.** If `kmalloc`/`kfree` from
   multiple CPUs can corrupt the kernel free list, and a corrupted
   allocation later backs a page-table page or a VMA struct → user
   page tables get scribbled. `DEBUG_HEAP=1` + the redzone check is
   the direct test.
6. **`sched_post_switch` / `prev_pending` / `on_cpu` under a
   3-way race.** The comments in `sched.c` describe fixing this for
   work-stealing; .NET's thread count is higher and the churn
   faster than anything that exercised it before. The `on_cpu`
   panic (`"scheduling a thread that is still on another cpu"`)
   would fire if this broke — check it hasn't been seen; if the
   machine hangs instead of panicking, the assertion may be racing
   too.

### 1e. Data flow trace

`cr2` → which VMA should cover it → which syscall last touched that
VMA → what else ran on other CPUs in that window (from the ring
buffer). Trace *backward* from the bad write to the operation that
un-mapped or mis-mapped the page. Fix at that source, not at the
fault.

## Phase 2 — Pattern analysis

- Compare NeoOS's `mm` locking to a known-correct reference:
  Linux's `mmap_lock` (formerly `mmap_sem`) rules — every VMA
  read under `mmap_read_lock`, every modification under
  `mmap_write_lock`, page-table walks that can race a teardown use
  `pte_offset_map_lock` / RCU. NeoOS almost certainly has a coarser
  single `mm_lock`; the question is whether *every* relevant path
  actually takes it. List every function that touches
  `proc->vmas` or `proc->pml4_phys`'s tables and check.
- Compare NeoOS's TLB shootdown to Linux's `flush_tlb_mm_range` /
  the `tlb_gather` batching + the "free pages only after the flush"
  ordering.

## Phase 3 — Hypothesis & minimal test

Form ONE hypothesis (e.g. "`sys_munmap` frees frames before the
cross-CPU TLB flush completes when the deferred-free queue is
full"). Make the smallest change that tests it (e.g. force the
deferred path always; or add the missing "wait for ACKs before
`pmm_free`"). Re-run the C stress test — the fast oracle — then the
.NET webtest concurrent load.

## Phase 4 — Fix at the source + regression

- The C pthread stress test becomes a permanent `userland/` selftest
  wired into the gauntlet (`[mmstress] PASSED` marker) so this class
  of bug can't regress silently.
- `-smp 4` .NET webtest: 200-request run at concurrency 8, zero
  failures, zero crashes — the acceptance bar.
- Full `tools/gauntlet.sh 15 3` at zero retries.
- If 3+ hypotheses fail: the `mm` locking model itself is the
  problem (too coarse to be correct, or too coarse to be fast
  enough and someone narrowed it unsafely) — escalate to an
  `mm`-locking redesign spec rather than patching symptom #4.

## Relationship to the other specs

- **MSC-1** (missing syscalls) should land first: `sched_setaffinity`
  lets the crash be reproduced with the *default* .NET publish
  (Server GC) instead of the workstation-GC workaround, and
  `eventfd2` removes one variable (.NET's engine falls back to
  different wake mechanics without it).
- **SCH-1/SCH-2** (scheduler) touch `sched.c` and the
  migration/`on_cpu` paths heavily. Either this investigation
  finishes first and SCH-2 builds on a fixed invariant, or SCH-2's
  own selftests (8 CPU-bound tasks, producer/consumer, idle-pull
  latency) are extended to include a "many threads of one process
  hammering shared memory" case and the two efforts merge.
