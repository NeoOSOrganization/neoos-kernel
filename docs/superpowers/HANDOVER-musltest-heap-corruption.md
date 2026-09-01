# HANDOVER — intermittent MUSLHELO heap-corruption crash (unsolved)

**Status:** NOT fixed. Root cause not found after ~15 instrumented repro cycles.
One real-but-insufficient bug was fixed along the way (see "Fix already applied").

**Author of this note:** Claude Sonnet 5 session `session_01CNR4gEkyMq6qhFWfxt3KXE`.
Handing to a fresh session / stronger model.

---

## TL;DR

`userland/musl/hello.c` (built as `/BIN/MUSLHELO.ELF`, the `[musltest]` test)
does `heap = malloc(4096); memset(heap,'x',4095); … ; free(heap);`. **`free()`
SIGSEGVs inside musl mallocng** (`nontrivial_free` / `get_meta`, RIP
`~0x2000_0000_2c5x` in the ELF, `cr2` = a tiny value like `0x1d` or a wild
address) because **the mallocng group's out-of-band metadata (`group->meta`
pointer) has been corrupted between the `memset` and the `free`**. The 4095-byte
payload itself is always intact — only the allocator metadata is hit.

NeoOS reports a signal-kill as `[process] task exited … code=0x0` (it does NOT
encode `128+sig` like Linux), so it looked like a clean exit with lost stdout.
The result is the gauntlet's `[musltest] ALL PASSED` marker going missing.

- **Rate:** ~7% under the standard gauntlet (`CONC=2`), ~35–40% at `CONC=4–5`,
  up to 97% when the mmap fault/unmap path is artificially slowed with
  instrumentation. Rate is dominated by host CPU contention → guest scheduling
  jitter. It is a **race**.
- **SMP-only.** Needs `-smp 4` (all repros used it). Single-CPU almost certainly
  never hits it.
- **Pre-existing.** A 15-run gauntlet on parent commit `2b347cb` (before any
  M1c-3 code) fails identically. It is **NOT** caused by the M1c-3 VT layer.
  The `2b347cb` commit message's "gauntlet 15/15" was luck on a small sample.
- Framebuffer / VT-render independent (parent failed in an 80x25 VGA-text run
  too; fbcon glyph math was checked and does not overflow the framebuffer).

---

## What has been RULED OUT (with evidence)

1. **Not a musl payload overrun.** `memset(heap,'x',4095)` into a mallocng slot
   that is actually ≥5440 bytes (size class 340 units). Instrumented dump right
   before `free`: `payload corrupt bytes = 0` in every run, good or bad.

2. **Not pmm frame double-free / reuse via the buddy allocator.**
   - Added a `frame_refcount==0` check in `pmm_free_locked` and a
     `refcount!=0` check in `pmm_alloc_locked`: **never fired**.
   - Added a targeted tracker (`pmm_dbg_track`/`untrack`): every mmap-region
     leaf frame a live process faults in is recorded; `pmm_free` / `pmm_alloc`
     shout if they touch a still-tracked frame. **`[DBGTRK]` never fired**, even
     though that same instrumentation drove the failure rate to 97%.
   - So MUSLHELO's group **frame is not being recycled** out from under it.

3. **Not a same-VA double demand-fault.** Added an "already present" guard in
   `vma_fault_locked` (re-check PTE under `p->mm_lock`, return success if
   present): **never fired**. Also: `#PF` uses an IDT **interrupt gate**
   (`idt.c`, type `0x8E`) so the fault handler runs `IF=0` and cannot be
   preempted between fault entry and `vma_fault`'s lock. A single-threaded
   process cannot double-fault the same page.

4. **Not the non-atomic `p->live_threads--` (though that WAS a real bug —
   fixed, see below). Fixing it did not change the rate.**

5. **Not `vma_mprotect_locked`'s `unmap_range` on a less-permissive mprotect**
   (which IS a latent correctness bug — see "Other bugs found"). Live trace of
   mallocng's actual syscalls shows the ONLY mprotect in the `malloc(4096)`
   path is `PROT_NONE → PROT_READ|WRITE` (more permissive), which is a no-op in
   that code. No RW→RO mprotect of a written page happens here.

6. **`[DBGFAS]` detector** (log when `free_address_space` runs while another CPU
   is `current` in that same `pml4_phys`): fires constantly in BOTH good and
   bad runs (mostly the self-CPU and siblings executing kernel exit code) —
   not discriminating. Inconclusive, probably benign.

---

## The exact mallocng syscall sequence (from a live boot, `DBGVM` trace)

```
mmap(len=0x2000, prot=PROT_NONE)        -> 0x500000000000   (meta arena: pg0 guard, pg1 storage)
mprotect(0x500000001000, 0x1000, RW)                        (splits the vma; MORE permissive => no-op on PTEs)
mmap(len=0x4000, prot=RW)               -> 0x500000002000   (the 4096-alloc's "group")
   -> vma_insert MERGES this with the mprotect'd [0x1000,0x2000) RW vma
      => one vma [0x500000001000, 0x500000006000) RW
memset + printf("[musl] malloc+memset ok")   <-- the printf is a writev syscall = a preemption/migration point
free():
   get_meta + nontrivial_free checks  <-- **SIGSEGV happens here in a bad run**
   munmap(0x500000002000, 0x4000)      (frees the group; correctly trims the merged vma back to [0x1000,0x2000))
```

- `heap` = `0x500000002020` (constant across all runs).
- `group` struct at `0x500000002000`; `group->meta` (first 8 bytes) points to
  `0x500000001018` — inside the mprotect'd meta page.
- Good-run dump of `group` header (`0x500000001fe0 + 0x20`):
  `18 10 00 00 00 50 00 00` = valid pointer `0x0000500000001018`.
- Bad runs: those 8 bytes read as mostly-zero / garbage → `get_meta` returns a
  ~0 pointer → deref faults at `cr2≈0x1d`.

Every vma operation above was traced by hand and resolves **correctly** (the
merge is legitimate; the munmap trims to exactly `[0x1000,0x2000)` keeping the
meta page mapped). No over-unmap.

---

## Key triangulation for the next investigator

Given:
- MUSLHELO is **single-threaded** (musl static, no `pthread_create`, no
  `clone`). Its own vma list / page tables are only ever touched by its one
  thread → serialized.
- The group **frame is not recycled** (tracker proved it).
- The corruption is in the **first ~8–16 bytes of the frame** (the mallocng
  `struct group` header, which is at a page boundary `0x500000002000`).
- Slowing the **mmap fault + unmap path** (adding locks/loops there) takes the
  rate from ~37% to ~97%.

…the corruption must be **either**:

- **(a)** the kernel writing to `phys_to_virt(<MUSLHELO's group frame>)** — a
  kernel bug that computes a wrong physical address and clobbers a live user
  frame via the physmap alias. Candidates: the buddy allocator's
  `list_push`/`list_remove` write `next/prev` into `phys_to_virt(block)` — if
  `buddy_frame` in `pmm_free_locked`'s merge loop is ever miscomputed to equal a
  live frame whose `frame_order[]` is stale-non-`ORDER_NONE`, `list_remove`
  writes 16 bytes of list pointers into it. **This matches the symptom exactly
  (first 16 bytes → pointer-shaped garbage / zeros).** I could not construct the
  stale-`frame_order` interleaving by inspection, but the pmm state is only
  ~partially convincing as fully race-safe; look hard at `frame_order[]`
  bookkeeping for frames in the *middle* of a multi-frame allocation and at the
  split-buddy `list_push` path. NOTE the tracker only watched *leaf data
  frames*, NOT page-table frames — a **page-table** frame of MUSLHELO being
  freed+reused would corrupt MUSLHELO's PTEs and is NOT covered by any evidence
  above.

- **(b)** MUSLHELO's PTE for `0x500000002000` being transiently repointed. For a
  single-threaded process this needs another agent touching its private page
  tables — e.g. `free_address_space` / `exec_task` of *another* process walking
  the wrong `pml4_phys`, or a shared lower-level table. Check whether any
  intermediate table (PDPT/PD/PT) can ever be shared between address spaces, and
  whether `alloc_table_frame()` (→`pmm_alloc`) can hand back a frame that is
  still a live page table.

### Recommended next step (not yet done)

Add a **physical-frame watchpoint**: when `vma_fault_locked` maps the page for
`page_down(addr) == 0x500000002000`, record the frame phys + snapshot its first
32 bytes. Poll it from `timer_handler` on every CPU every tick; on any change
after the initial write, log `change#, phys, this_cpu index, current->proc->pid,
new bytes`. This catches the writer regardless of mechanism and names the CPU /
process running when it happens. (I had this ~80% coded in
`kernel/mm/paging.c` as `paging_dbg_watch` / `paging_dbg_watch_tick` +
`paging.h` decls + a call site in `vma_fault_locked` + a call from
`timer_handler`, then reverted it to write this doc. Re-create it.)

Also worth trying: **`-smp 1`** repro (expect 0% — confirms SMP race), and
building MUSLHELO's `hello.c` to spin in a tight loop (no syscalls) between
`malloc` and `free` (expect ~0% — confirms the printf/writev preemption is the
trigger window).

---

## Fix already applied (KEEP IT — real bug, just not THE bug)

`kernel/sched/proc.c` + `kernel/sched/thread.c` — `p->live_threads` made atomic:

- `proc_get_live` / `thread_alloc`'s increment → `__atomic_add_fetch(…, ACQ_REL)`
- `proc_put_live`'s `if (--p->live_threads > 0) return;`
  → `if (__atomic_sub_fetch(&p->live_threads, 1, __ATOMIC_ACQ_REL) > 0) return;`

**Why:** `process_exit()` SIGKILLs all sibling threads; they run `proc_put_live`
concurrently on several CPUs. The non-atomic `--` lets the last two both read 1
and write 0 → `free_address_space()` runs **twice** → every frame `pmm_free`'d
twice. `threadtest` ("exiting with a blocked sibling") and `mpitest` exercise
this right before MUSLHELO in the boot order. This is a genuine double-free bug
and the fix is correct — it just did not move the failure rate, so there is a
second, independent cause.

`git diff` is clean except these two files (+ the pre-existing dirty
`third_party/musl` which must NOT be touched).

---

## Other bugs found in passing

**Bugs 1–3 below are now FIXED** (see "Follow-up fixes" at the end of
this file). Bug 4 is untouched. The main MUSLHELO corruption above is
still open; none of the three fixes changed its rate either way, though
bug 1's fix means the physmap now has real TLB shootdowns and real frame
reclamation happening on paths that previously did neither, which
changes the timing the race lives in.

1. **`tlb_shootdown()` is never called after boot.** It is invoked ONLY from
   `tlb_shootdown_selftest()` at boot. `tlb_flush_deferred()` (which returns
   deferred frames to the pmm) runs ONLY inside `tlb_shootdown()`. Therefore
   **every frame freed via `paging_unmap_from(free_frame=1)`** (all `munmap`,
   `mprotect`-shrink, `thread_stack_free`, `vma_destroy_all` on exit) is
   `tlb_defer_free`'d and **never reclaimed** — a permanent leak. `munmap` /
   process-exit do no TLB shootdown at all.

2. **`vma_mprotect_locked()` (`kernel/mm/vma.c` ~line 224):** making a range
   non-writable (`!(prot & PROT_WRITE)` — **includes `PROT_READ`!**) does
   `unmap_range(..., free_frames=1)` — it **discards page contents and frees the
   frames**. `mprotect(addr,len,PROT_READ)` must preserve contents. mallocng
   doesn't hit this in the `malloc(4096)` path, but a real ported program will.
   Fix: rewrite present PTEs with reduced permission bits, keep the frame.

3. **`exec_task()` does not terminate sibling threads before
   `free_address_space(old_pml4)`.** Linux `execve()` kills all other threads
   first. If a multi-threaded process ever execs, siblings keep running user
   code in the just-freed address space. (Test suite may not currently trigger
   multi-threaded exec.)

4. **`proc_get_live()` is dead code** — declared, defined, never called. (The
   first thread's `live_threads` count comes from `thread_alloc`'s increment.)

---

## Repro harness

`bash .superpowers/sdd/2026-08-31-phase14-input-and-solidity/pgauntlet.sh N CONC`
runs N production boots, CONC at a time, and greps `REQUIRED_MARKERS`. A run
that's missing `[musltest] ALL PASSED` with no FAILED/PANIC is classed "flaky"
and retried solo once; it only hard-fails the gauntlet if the retry also misses.

Faster tight loop used during this investigation (writes per-run serial to
`scratchpad/repro/serial.N`):

```bash
# scratchpad/reprodbg.sh — boots build/{neoos.iso,disk.img,disk2.img} N times,
# CONC at a time, 90s timeout each, reports which runs are missing the marker.
```

- `make clean-kernel iso disk-image` builds the **production** image (no
  `-DNEOOS_TEST_HOOKS`). Do NOT run `make` while a gauntlet/repro loop runs
  (shared `build/`).
- `make test` is the single-boot test-hooks path; it rarely repros (no
  contention).
- Instrumenting `userland/musl/hello.c`: add `#include <sys/syscall.h>` and use
  `syscall(SYS_write, 2, buf, n)` for trace that bypasses musl stdio. **Buffer
  it and flush in ONE write** — per-line raw writes add syscalls to the window
  and change the rate. Revert hello.c before finishing (it is a checked-in
  test).

---

## Constraints (from CLAUDE.md / user)

- Work on `main`, no feature branches.
- `third_party/musl` stays dirty (pre-existing) — never touch it.
- Commit trailer:
  `Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>` /
  `Claude-Session: https://claude.ai/code/session_01CNR4gEkyMq6qhFWfxt3KXE`
- Milestone roadmap that this interrupts: `docs/superpowers/plans/2026-09-01-m1c-3-kernel-vts.md`
  (M1c-3 Task 2 committed as `5922fb2` and verified NOT a regression;
  Task 3 = `/dev/tty1..6` + VT/KD ioctls + `Alt+F1..F6` intercepts is next once
  this bug is closed). Memory note: `musltest-flake-post-kmsg.md`.

---

## Follow-up fixes (bugs 1–3 above)

Fixed and verified by QEMU boot (`make test`) and a 15-run production
gauntlet at `CONC=2` (15/15).

### 1. Deferred frames were never reclaimed, and nothing ever shot down

`paging_unmap_from(free_frame=1)` does not call `pmm_free` — it calls
`tlb_defer_free`, and the deferred queue is drained by exactly one
thing, the tail of `tlb_shootdown()`. Since the only `tlb_shootdown()`
call was the boot selftest, **every** frame ever `munmap`'d, mprotect'd
away or released with a thread stack leaked permanently, and no unmap
after boot ever invalidated another CPU's TLB.

- Shootdowns added at every point that unmaps, always with `mm_lock`
  released (`tlb_shootdown` panics if any lock is held): `vma_munmap`,
  `vma_mprotect`, `vma_mmap` (MAP_FIXED only), `vma_map_phys`'s rollback,
  `vma_destroy_all`, `thread_stack_free` and `thread_stack_alloc`'s
  rollback.
- `tlb_shootdown()` now drains the deferred queue **only when called
  with `pml4_phys == 0`**. The queue is global, so a shootdown targeted
  at one address space must not release another's frames — that was
  unsound even in the original code. All the new callers pass 0.
- `free_address_space()` now `tlb_defer_free`s instead of `pmm_free`ing.
  It could not shoot down where it is called from: `proc_put_live()`
  runs inside `thread_exit_self` with interrupts off on a thread already
  marked `ZOMBIE`, and `tlb_shootdown` must enable interrupts to wait
  for acks — being preempted there would park the thread forever with
  the address space half-freed. `proc_reap()` performs that shootdown
  instead, once every thread of the process is provably off-CPU. This
  also closes a real hazard: page-table frames of a dying process used
  to go straight back to pmm while a killed-but-not-yet-descheduled
  sibling still had them in CR3.

Regression test: `mmaptest` now maps, touches and unmaps 225 MiB in
900 rounds — far more than the machine has. It exhausts memory without
reclamation and passes with it.

### 2. `mprotect` destroyed page contents

`vma_mprotect_locked` called `unmap_range(..., free_frames=1)` whenever
the new prot lacked `PROT_WRITE` — which includes plain `PROT_READ`. It
now rewrites the leaf PTEs in place via a new
`paging_setprot_from()`, keeping the frame:

- The frame address, `PAGE_NOFREE` and `PAGE_COW` survive; only the
  access bits change.
- `PROT_NONE` is stored the Linux way — `PAGE_PRESENT` cleared, the
  frame kept, and a new software bit `PAGE_PROTNONE` (bit 11) marking
  the entry as still owning a frame. `free_address_space`,
  `paging_unmap_from` and `fork` all test `PAGE_HAS_FRAME` now, so a
  `PROT_NONE` page is neither leaked nor lost across a fork.
- Widening is applied here too rather than deferred to the next fault,
  because a page being made writable again is already present and would
  never fault. `paging_setprot_from` refuses `PAGE_WRITABLE` to a
  `PAGE_COW` page, so fork's sharing survives an `mprotect`.

Regression test: `mmaptest` writes a pattern, `mprotect`s RW → RO →
RW → NONE → RO and checks every byte at each step, plus that the
protections actually bite (a write after RO and a *read* after NONE both
SIGSEGV in a forked child).

### 3. `exec` did not terminate sibling threads

`exec_task()` freed the old address space with every other thread of the
process still executing user code in it. It now calls
`exec_reduce_to_one_thread()` first — but only after the new image has
been built and validated, so a failing exec still leaves the caller
completely unchanged.

The obvious implementation does not work: `thread_kill()` sends SIGKILL,
whose default action is `signal_terminate` → `process_exit`, so it takes
the whole process down (observed as the exec'ing process dying with
status `128+9`). A new `thread_kill_solo()` sets `t->solo_kill` before
sending, and `deliver_one()`'s fatal-default branch honours it by
calling `thread_exit_self(0)` instead of `signal_terminate`.

The wait is the part that matters: exec spins on `p->live_threads > 1`
calling `schedule()` — a killed thread parks itself on `p->zombies`
before it drops `live_threads`, so the count reaching one means every
sibling is past that point and off its CPU. Their `stack_slot`s are then
cleared, because exec resets the slot bitmap and a later reaper would
otherwise unmap those addresses out of the *new* address space.

Regression test: `threadtest` forks a child, starts three threads that
spin writing their own stacks, execs `/BIN/EXECTARG.ELF`, and requires
exit status 0.

### 4. `proc_get_live()` is dead code — still true, still not filed.
