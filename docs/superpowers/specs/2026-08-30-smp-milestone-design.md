# SMP & Concurrency (Phase 10) — Design

**Date:** 2026-08-30
**Status:** approved, pending implementation plan
**Predecessors:** Phases 1–8 (`ff1df54`). Phase 9 (source reorganization)
is closed as already-satisfied: `kernel/` is split into `fs/ mm/ sched/
sync/`, which was that phase's entire content.

---

## 1. Problem

NeoOS runs on exactly one CPU. `MAX_CPUS` is 1, `MAX_TSS` is 1, the
MADT parser reads IOAPIC and interrupt-source-override entries but
skips Local APIC entries entirely, and no application processor is ever
started.

The single-CPU assumption is not merely a missing feature — it is load
bearing. Six of the fourteen declared lock ranks name locks that do not
exist:

| Rank | Subsystem | Lock calls in the code |
|------|-----------|------------------------|
| `LOCK_RANK_MOUNTTABLE` | `fs/vfs.c` | 0 |
| `LOCK_RANK_VNODEHASH` | `fs/vfs.c` | 0 |
| `LOCK_RANK_VNODE` | `fs/vfs.c` | 0 |
| `LOCK_RANK_RUNQUEUE` | `sched/sched.c` | 0 |
| `LOCK_RANK_HEAP` | `mm/heap.c` | 0 |
| `LOCK_RANK_PMM` | `mm/pmm.c` | 0 |

`mm/paging.c`, `mm/vma.c` and `ata.c` are likewise unlocked, and
`waitq`'s `head`/`tail` are protected only by whichever guard lock the
caller happens to hold — a different lock for different callers.

The lock-rank table in `OPTIMIZATION_SUMMARY.md` therefore describes an
aspiration, not the kernel. Phase 7's "per-CPU ready queues" are per-CPU
in layout only. This milestone makes both real.

## 2. Scope

In scope:

1. CPU discovery via MADT, application-processor bringup, per-CPU GDT
   TSS descriptors, per-CPU idle threads.
2. Fine-grained locking of all six unlocked subsystems plus `waitq`,
   introduced from the start rather than staged behind a big kernel
   lock.
3. Locked per-CPU ready queues with work stealing.
4. Three IPIs: reschedule, TLB shootdown, panic-stop.
5. `sysconf(_SC_NPROCESSORS_ONLN)` and `sched_getcpu()` exposed through
   the musl shim, per the standard-library convention.
6. A refreshed `docs/abi-compatibility.md` (created by this milestone)
   as the closing deliverable.

Out of scope: NUMA awareness, CPU hotplug, priority or fairness changes
to the scheduling policy, per-CPU slab caches. `MAX_CPUS` is 8.

### 2.1 Decisions taken, with the alternatives rejected

- **Fine-grained locking from the start**, not a big kernel lock peeled
  apart later. Accepted risk: the first SMP boot exercises every new
  lock at once, so a deadlock has no bisection point. Mitigated by the
  rank checker, which converts an ordering error into an immediate
  panic naming both locks rather than a hang.
- **Per-CPU queues with work stealing**, not push balancing and not a
  global queue. Validates the Phase 7 layout rather than discarding it,
  and keeps the common case lock-local.
- **All three IPIs**, including panic-stop. Shootdown is a correctness
  requirement, not an optimization.
- **All six lock layers in one milestone**, ordered by blast radius. A
  partial job is a kernel that corrupts its own free lists.

## 3. CPU discovery and topology

`parse_madt` gains a type-0 (Local APIC) branch, filling
`info->cpus[]` with `{acpi_id, lapic_id, enabled}` for every entry whose
flags have bit 0 (Enabled) or bit 1 (Online Capable) set.

`MAX_CPUS` becomes 8; `MAX_TSS` follows it.

`cpus[]` is indexed by a **dense NeoOS CPU index**, never by `lapic_id`
— APIC ids are sparse and can exceed `MAX_CPUS` on real hardware. A
`lapic_id → cpu_index` lookup serves the reverse direction for IPI
targeting.

`cpu_local_init()` splits:

- `cpu_local_init_bsp()` — index 0, today's behaviour.
- `cpu_local_init_ap(index)` — sets `self`, `tss = &tss[index]`,
  `lapic_id`, and zeroes the queue and lock-tracking fields.

The ordering constraint in today's comment still holds and still
matters on every CPU: this runs **after** the GDT reload, because
`mov gs, ax` zeroes `IA32_GS_BASE` as a side effect.

Each CPU gets its own idle thread. `idle_init()` becomes per-CPU, and
the idle thread's reserved `tid` becomes `-(index + 1)` so idle threads
remain distinguishable from each other and from real threads in the
serial log.

## 4. Application-processor bringup

New files: `kernel/smp.c`, `kernel/smp.h`, `kernel/ap_trampoline.asm`.

### 4.1 Trampoline placement

The trampoline assembles at `org 0x8000` into its own section, and
`smp_init()` copies it to physical `0x8000` at boot. That address is
safe by construction: `pmm_init` never hands out memory below
`0x100000` (`mm/pmm.c:205`), so nothing can claim the page.

### 4.2 What the trampoline does

It is `boot/boot.asm`'s long-mode path with the page-table
*construction* removed. The low 4 GiB identity map built at boot is
permanent — `mm/paging.c` keeps `PML4[0]` because pmm dereferences
through it — so the AP can simply adopt the live `p4_table`:

1. 16-bit real mode: load a tiny GDT reachable from the trampoline page,
   enter 32-bit protected mode.
2. Set `CR3 = p4_table`, enable PAE, set EFER.LME, enable paging.
3. Far-jump to 64-bit, load the real kernel GDT, and indirect-call
   `ap_main` (a `rel32` call cannot reach the higher half — the same
   constraint `long_mode_start` already documents).

Two values are patched into the trampoline page before each SIPI: the
AP's stack pointer and its CPU index.

### 4.3 Serialized bringup

The BSP starts **one AP at a time**, spinning until that AP sets
`cpus[i].online` before moving to the next. Parallel bringup would have
several APs sharing the trampoline's single stack slot. Serializing
costs milliseconds once per boot and removes the entire class of bug.

Per AP: write the warm-reset vector, INIT assert, INIT deassert, wait
10 ms, SIPI with vector `0x08`, wait 200 µs, second SIPI, then wait up
to 100 ms for `online`. **A CPU that never comes online is logged and
skipped, not fatal** — the kernel continues with fewer CPUs.

### 4.4 `ap_main(index)`

`gdt_load()` (shared GDT; `ltr` its own selector) → `idt_load()` (the
IDT is already built and is shared) → `cpu_local_init_ap(index)` →
`lapic_init()` on its own LAPIC → `cpu_init()` for SSE/xstate → allocate
its idle thread → mark `online` → `sti` → `schedule()`.

### 4.5 GDT

`gdt_entries` grows to hold one TSS descriptor per CPU (indices
`3 + 2*i`, since a 64-bit TSS descriptor occupies two slots). All CPUs
load the same GDT and differ only in the selector passed to `ltr`. A
per-CPU GDT was considered and rejected as `MAX_CPUS` copies of a table
identical but for one descriptor.

### 4.6 Placement in `kmain`

After `process_init()` and after the userland spawns, immediately before
the scheduler starts. Every subsystem an AP can touch is initialized and
locked by that point.

## 5. Locking

### 5.1 Rank table change

Two insertions; every existing *relative* order survives, so no
currently-correct acquisition sequence becomes an inversion:

```
LOCK_RANK_PROCTABLE    (0)
LOCK_RANK_PROCESS      (1)
LOCK_RANK_THREAD       (2)
LOCK_RANK_MM           (3)   NEW — per-process address space (vma + pml4)
LOCK_RANK_MOUNTTABLE   (4)   } shifted +1 by the MM insertion
LOCK_RANK_VNODEHASH    (5)   }
LOCK_RANK_VNODE        (6)   }
LOCK_RANK_BLOCKDEV     (7)   }
LOCK_RANK_DRIVER       (8)   }
LOCK_RANK_WAITQ        (9)   NEW — per-wait-queue
LOCK_RANK_RUNQUEUE    (10)   } shifted +2 (MM and WAITQ both above)
LOCK_RANK_FDTABLE     (11)
LOCK_RANK_HEAP        (12)
LOCK_RANK_PMM         (13)
LOCK_RANK_SIGQUEUE    (14)
LOCK_RANK_SERIAL     (255)
```

`LOCK_RANK_MM` sits low, not next to `HEAP` where its allocation
behaviour superficially suggests, for two reasons:

- A demand-paging fault takes the address-space lock and then reads
  through the filesystem, so `MM` must be **above** nothing in VFS and
  **below** `MOUNTTABLE`.
- Such a fault can sleep, so `MM` must be below `RUNQUEUE`.

Placing it near `HEAP` inverts on the first mmap-backed page fault.

`LOCK_RANK_WAITQ` sits above every lock that is legally held across a
`waitq_sleep()` call — a mutex passes its own guard in as `release`, and
those guards carry the mutex's rank (`MOUNTTABLE`, `VNODE`,
`BLOCKDEV`) — and below `RUNQUEUE`, since the sleep path reaches
`schedule()` after the wait queue is updated.

### 5.2 The `schedule()` invariant

`waitq_sleep` releases the caller's guard **before** calling
`schedule()` (`kernel/waitq.c:63`), so `schedule()` is always entered
holding zero spinlocks. This is currently an accident of the code; it
becomes an enforced invariant:

```c
if (lock_held_depth() != 0) { lock_panic("schedule with lock held", ...); }
```

It is cheap, and it converts an entire class of four-CPU deadlock into
an immediate panic naming the offending lock.

### 5.3 The six layers, in implementation order

Ordered by blast radius so each addition is independently bootable.

1. **pmm** — one `LOCK_RANK_PMM` spinlock over the free list. A leaf:
   it allocates nothing beneath itself.
2. **heap** — one `LOCK_RANK_HEAP` spinlock, taking pmm beneath it,
   which is the already-declared order.
3. **runqueue** — a `LOCK_RANK_RUNQUEUE` spinlock per `struct cpu`.
4. **waitq** — a `LOCK_RANK_WAITQ` lock per queue, replacing today's
   implicit "the caller's guard covers it". Different callers hold
   different guards, so today's arrangement protects nothing across
   CPUs. This is a latent bug being fixed, not a nicety.
5. **vfs** — mount table, vnode hash buckets, per-vnode locks.
6. **paging/vma** — a per-process `mm_lock` at rank 3; and **ata** at
   `LOCK_RANK_DRIVER`, since PIO port sequences issued from two CPUs
   interleave into garbage.

### 5.4 Work stealing and the equal-rank problem

A CPU whose queue is empty steals from the longest remote queue. New
threads are placed on the least-loaded CPU at creation.

Stealing takes two locks of **equal** rank (both `RUNQUEUE`), which the
checker forbids — deliberately, since equal ranks are how inversions
start. Rather than weaken that rule, a narrow

```c
void spin_lock_ordered_pair(struct spinlock *a, struct spinlock *b);
```

acquires both in **CPU-index order** (lower index first) and tells the
checker it has done so. Every CPU pair is therefore acquired in one
consistent global order, which is sufficient to prevent a cycle.

## 6. IPIs

Vectors `0x20` (timer) and `0x21` (keyboard) are taken and `0xFF` is
spurious; all 256 IDT stubs already exist (`kernel/isr.asm:126`), so
IPI vectors need only handlers registered.

### 6.1 Reschedule — `0xF0`

Sent when a thread becomes runnable on a CPU halted in `idle_entry`'s
`sti; hlt`, and when a queue grows past a threshold while another CPU is
idle. The handler only issues `lapic_send_eoi()`; the interrupt itself
breaks the `hlt`, and the idle loop reaches `schedule()` on its next
pass.

### 6.2 TLB shootdown — `0xF1`

Two CPUs running threads of one process share a page table, so an unmap
on one leaves a stale TLB entry on the other. Without this, `mmaptest`
and COW fork corrupt memory.

**The deadlock hazard, and why the design avoids it structurally.**
`spin_lock_irqsave` clears IF, so a CPU spinning on a lock cannot take
an IPI. If the sender holds `mm_lock` while spinning for
acknowledgements, and the target is spinning on that same `mm_lock`,
neither ever moves.

The sequence is therefore:

1. Modify the page table **under** `mm_lock`.
2. Record the range in a per-CPU shootdown descriptor.
3. **Release `mm_lock`.**
4. Send the IPI and wait for acknowledgements **with IF enabled**.

**The correctness rule this creates:** a physical frame is returned to
pmm only **after** every acknowledgement is in. A stale TLB entry
pointing at a page the address space still owns is harmless; one
pointing at a page pmm has already re-handed to another process is
memory corruption. `paging_unmap_from(..., free_frame=1)` and the COW
fault path both change to defer their `pmm_free` behind the shootdown.

Targeting is narrowed to CPUs whose `current->proc->pml4_phys` matches
the address space being modified, so a shootdown for a single-threaded
process usually sends zero IPIs.

### 6.3 Panic-stop — `0xF2`

Delivered as an **NMI**, not a maskable vector: the entire purpose is to
stop CPUs spinning with interrupts disabled, which a normal vector
cannot do. `lock_panic()` and the fault handlers broadcast it before
printing. The handler halts forever **without touching the serial lock**
— the panicking CPU may be holding it.

## 7. Testing

`make run` gains `-smp 4`.

Per the Phase 5c lesson, each new selftest must be shown to **fail on
the pre-change kernel** before the phase is called complete.

1. **`smp_selftest_parallel`** — 8 kernel threads each increment a
   shared counter 10,000 times under one spinlock, recording
   `this_cpu()->lapic_id`. Asserts the exact total (a broken spinlock
   loses increments) **and** that ≥2 distinct lapic ids did work. The
   second assertion fails on today's kernel.
2. **`smp_selftest_steal`** — enqueue N threads onto CPU 0's queue only;
   assert every CPU eventually runs at least one. Fails on a kernel
   whose APs come online but whose stealing is broken — precisely what
   a boot-only test misses.
3. **`smp_selftest_shootdown`** — one thread maps and writes a page; a
   second thread on another CPU reads it after the first unmaps and the
   frame is recycled. Asserts the reader faults rather than observing
   recycled data. Fails without the deferred-`pmm_free` rule of §6.2.

Regression gate: all five userland suites (vfstest, avxtest, mmaptest,
threadtest, sigtest) and every existing kernel selftest pass under
`-smp 4`.

## 8. Userland surface

Per the standard-library convention, SMP makes CPU count and current
CPU observable from userland, so this milestone owes library support
rather than a raw syscall number:

- `sysconf(_SC_NPROCESSORS_ONLN)` and `sysconf(_SC_NPROCESSORS_CONF)`
- `sched_getcpu()`

Both are routed through the musl shim onto NeoOS syscall numbers, with
Linux-matching return shapes. `docs/stdlib.md` gains an entry.

## 9. Closing deliverable: ABI compatibility report

Per the Linux ABI convention in `CLAUDE.md`, this milestone **creates**
`docs/abi-compatibility.md`, covering the state of the boundary as a
whole, not only what Phase 10 added:

- syscalls implemented, stubbed, and absent, against the Linux x86_64
  set that a ported application would reach for
- struct layouts crossing the boundary (`stat`, `dirent`, `timespec`,
  `sigaction`, ...) and whether each matches Linux's x86_64 layout
- constant and flag values (`O_*`, `PROT_*`, `MAP_*`, `SIG*`, errno)
  and any that diverge
- the ELF entry / auxv / TLS / signal-frame contract
- every deliberate divergence, with its reason
- what a real ported application would still hit

## 10. Risks

| Risk | Mitigation |
|------|-----------|
| First SMP boot exercises every new lock at once (accepted consequence of fine-grained-from-the-start) | Rank checker panics naming both locks instead of hanging; layers land in blast-radius order so each is independently bootable |
| AP bringup hangs with no output | Bringup is serialized and per-AP bounded at 100 ms; a CPU that never comes online is logged and skipped |
| Shootdown deadlock | Structural: `mm_lock` released before the acknowledgement wait, which runs with IF enabled |
| Use-after-free via stale TLB | Frame freed only after all acknowledgements |
| A spinning CPU swallows a panic | Panic-stop is an NMI |
