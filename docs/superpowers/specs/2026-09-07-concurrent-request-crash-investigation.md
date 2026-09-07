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
