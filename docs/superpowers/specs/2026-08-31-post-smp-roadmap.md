# NeoOS roadmap

**Created:** 2026-08-31 (as the post-SMP driver roadmap)
**Rewritten:** 2026-09-01 — the driver and audio track was dropped in
favour of concurrency hardening. Filename kept so existing references
resolve.
**Status:** planning index. Each live milestone has (or gets) its own
design spec + implementation plan under `docs/superpowers/`.

## Where the project is

```
DONE
  A.     net socket-lifetime fix        commit a66e5f7
  B.     NX / W^X enforcement           commits 9cabab4..d872e7c
  SMP.   lifetime + lock detangle       refcounting in, kernel/sync/rcu.c deleted
  Ph14.  input + solidity               evdev, keyboard decoder, CSPRNG AT_RANDOM
  M1a.   console plumbing               /dev/fb0 + fbcon, poll/select, PTY, devfs_register
  M2.    init (PID 1)                   /SBIN/INIT from /ETC/INITTAB, reboot(2), reparenting
  M1b-1..3. framebuffer terminal        Spleen font, VT engine, TERM process
  M1c-1..3. driver model + kernel VTs   kernel/drivers reorg, fb_device + con_driver,
                                        /dev/tty1..6 with Alt+Fn, VT/KD ioctls
  BB0.   fork inherits the vma list     commits 080ce08, 8401c63
  CS.    Concurrency hardening & scaling
         CS0 audit · CS1 instrumentation · CS2 regressions · CS3 stress
         + chaos · CS4 fixed limits · CS5.2/5.3/5.4 lock architecture.
         CS5.1 (vfs_lock peel) and CS5.5 (seqlocks) deliberately NOT
         done -- see the results section of the spec.
         spec 2026-09-01-concurrency-and-scaling-design.md
  BB.    BusyBox (static) — BB0..BB6    an interactive ash with job
         control, pipes and redirection, on a pty. BusyBox 1.37.0 as a
         submodule, static against NeoOS's musl.
         plan plans/2026-09-01-busybox-track.md

LIVE
  C.     ASLR                                 ← NEXT
         mmap base, stack, brk, PIE load base from the Phase 14 CSPRNG,
         with a norandmaps off-switch. Untouched: it was sequenced
         before BusyBox and taken after it, because BusyBox was the
         milestone the roadmap existed to reach and ASLR is bounded
         hardening that nothing else waits on.
         spec 2026-08-31-aslr-design.md · plan plans/2026-08-31-aslr.md

DEFERRED
  DL.    Dynamic linking — DL1 file-backed mmap, DL2 PT_INTERP + full
         auxv, DL3 dynamic TLS + dlopen. Deferred, not cancelled: taken
         up after BusyBox. BusyBox is built static and never depended on
         it. Decomposition in
         2026-09-01-busybox-and-dynamic-linking-roadmap.md.

DROPPED — see "What was dropped" below
  D.     x2APIC
  E.     FDC (floppy) driver
  F.     Audio stack (F1 sound core, F2 SB16, F3 AC97, F4 Intel HDA)
  M1c-4. VT_PROCESS handshake + scrollback / evdev keys
```

## Why this order

**Concurrency first, and specifically before BusyBox.** The kernel is
SMP with work-stealing, but nothing in the tree can make a rare
interleaving happen on purpose — every SMP bug found so far was found
by luck. Two things force the issue now: the refcounted-lifetime
pattern landed but was never applied to `pid_alloc`, the last unsafe
"find in a table, drop the lock, use the pointer" site; and BusyBox
lands directly on the fixed caps that are still in place
(`SPAWN_MAX_ARGS 8`, `POLL_MAX_FDS 16`, the non-POSIX fd 0/1/2
allocator). BB0 already had to fix `fork` losing the vma list — a bug
the existing tests missed because they only touched pages the parent
had touched first. That is the class of bug CS exists to stop finding
by accident.

**ASLR was taken AFTER BusyBox, not before.** The reasoning below still
holds; the ordering did not survive contact. BusyBox is what the roadmap
exists to reach, ASLR is bounded hardening nothing else waits on, and
taking ASLR first risked spending the session without reaching the
destination. Recorded rather than quietly re-sequenced.

**ASLR after CS, before BusyBox.** It is bounded hardening of code that
already exists: NX/W^X has landed, and Phase 14 already built the
CSPRNG its entropy comes from. Doing ASLR without W^X first would be a
lock on a door with no wall; that ordering constraint is satisfied.

**BusyBox last.** It is the milestone that pays off the CLAUDE.md goal
— run real Linux applications unpatched — and it wants the syscall
surface to be stable and the caps to be gone before it starts.

**Dynamic linking after BusyBox.** Highest long-term leverage on
running unpatched binaries, but BusyBox is built static, so it is a
successor rather than a prerequisite.

## What was dropped, and why

Removed from the roadmap on 2026-09-01. Their specs and plans were
deleted in the same commit; `git log` recovers them if any is revived.

| # | Milestone | Files removed | Why dropped |
|---|---|---|---|
| D | x2APIC | `specs/2026-08-31-x2apic-design.md`, `plans/2026-08-31-x2apic.md` | Entirely latent benefit. `MAX_CPUS` is already 128 and QEMU runs 4; >255 logical CPUs and MSR-based IPIs buy nothing the project can currently observe. |
| E | FDC (floppy) | `specs/2026-08-31-fdc-driver-design.md`, `plans/2026-08-31-fdc-driver.md` | A cheap driver win with no consumer. ATA already covers the block seam FAT mounts through. |
| F | Audio stack | `specs/2026-08-31-audio-stack-design.md`, `plans/2026-08-31-audio-p1-core-and-sb16.md` | The largest remaining item — a new device class, a userland ABI decision (OSS vs ALSA) and three drivers — for a hobby kernel with no application that plays sound. Displaced by concurrency work that BusyBox actually needs. |
| M1c-4 | VT_PROCESS handshake, scrollback, evdev keys on the VT | (no separate files; described in `specs/2026-09-01-m1c-driver-model-and-vts-design.md`) | M1c-3 ships `VT_AUTO` switching, which is what an interactive shell on `/dev/tty1..6` needs. The `VT_SETMODE`/`VT_RELDISP`/`SIGUSR1` handshake only matters to a display server that wants to veto a switch — NeoOS has none. `VT_RELDISP` stays accepted-but-inert, as M1c-3 documented. |

These were **specced but never user-reviewed** — the note in the
original roadmap flagged that every design decision in them (OSS vs
ALSA, x2APIC-always vs opt-in, the `struct blockdev` seam) was the
executor's call, awaiting a review pass that never happened. Dropping
them costs no reviewed work.

## Cross-cutting constraints (all milestones)

- **No host unit tests.** Every test is a kernel selftest or a userland
  binary, verified by `make test` (headless QEMU, 4 CPUs) and the
  15-run parallel gauntlet
  (`tools/gauntlet.sh`,
  CONC=3, `PGAUNTLET PASSED: 15/15`). A single green `make test` is not
  sign-off for anything timing-dependent. CS1 promotes this script to a
  stable location and adds per-marker flakiness reporting.
- **Lock ranks are enforced.** Any new lock gets a `LOCK_RANK_*` slot
  in `kernel/sync/lock.h` with a written rationale; acquiring out of
  order panics the boot. 27 ranks defined today.
- **The ABI is not ours.** Anything a user program can observe — struct
  layouts, flag values, error codes, auxv entries, signal-frame layout
  — matches Linux x86-64. Syscall *numbers* stay NeoOS's own; the musl
  shim translates. Every deliberate divergence is recorded in
  `docs/stdlib.md`.
- **Every user-facing feature needs a musl path or a `lib/` wrapper**
  plus a `docs/stdlib.md` entry (CLAUDE.md).
- **QEMU is the reference machine.** `-cpu Nehalem -smp 4`.
- **Milestone close:** refresh `docs/abi-compatibility.md`, update
  `docs/stdlib.md`, and write the milestone's own summary.

## Milestone briefs

### CS. Concurrency hardening & scaling
Spec: `docs/superpowers/specs/2026-09-01-concurrency-and-scaling-design.md`

Six sub-milestones, each getting its own implementation plan when
reached:

- **CS0 audit** — does the new kernel-VT code (`kvt.c`, `console.c`,
  `con_driver.c`, which contain no locking at all) race console output
  on VT switch? And the five-file `spin_lock_raw` audit: `pty`,
  `devfs`, `fbcon`, `rand` all take properly-registered locks with the
  raw variant, which the rank checker cannot see; `rand_lock` also
  shares `LOCK_RANK_SERIAL` with the unrelated serial lock.
- **CS1 instrumentation** — poisoned/redzone heap mode, per-rank lock
  hold-time histograms, the gauntlet promoted and given per-marker
  flakiness reporting. Built first because the hardest bugs below need
  poisoning to be *localizable*, not merely detectable.
- **CS2 regressions** — the lockless `pid_lookup`, `select()` silently
  dropping past 16 ready fds, the waitq double-free, TLB-shootdown
  timeout and frame accounting, rank-checker coverage.
- **CS3 stress + chaos** — `forkstorm`, `mmapracer`, `faultflood`,
  `pollstorm`, `ptychurn`, `sigstorm`, `rankinvert`, plus
  `NEOOS_DEBUG_HZ`, `pmm_alloc` failure injection and fuzzed concurrent
  teardown.
- **CS4 fixed limits** — PTY pool, `spawn` argv, POSIX fd allocation,
  dynamic `poll`/`select`, `MAX_THREADS_PER_PROC`, PID wraparound.
- **CS5 lock architecture** — peel the global `vfs_lock`, replace the
  `poll_broadcast` thundering herd with a real poll table, CI checks
  for duplicate ranks and new raw locks, apply the refcount pattern to
  `pid_alloc`, then seqlocks for read-mostly table scans.

### C. ASLR
Spec: `docs/superpowers/specs/2026-08-31-aslr-design.md`
Plan: `docs/superpowers/plans/2026-08-31-aslr.md`

Randomize the mmap base hint, main-thread stack top, brk start and the
`ET_DYN` (PIE) load base, with a `norandmaps` off-switch (Linux's
`kernel.randomize_va_space`). Entropy bits chosen to match Linux
x86-64 defaults. Depends on NX/W^X, which has landed. The plan's Task 1
predates Phase 14's CSPRNG work — check what `AT_RANDOM` already draws
from before rebuilding a pool.

### BB. BusyBox (static)
Plan: `docs/superpowers/plans/2026-09-01-busybox-track.md` — the live
ordering, superseding the BusyBox half of
`specs/2026-09-01-busybox-and-dynamic-linking-roadmap.md`.

BB0 (fork inheriting the vma list) is done. BB1–BB4 are independent of
each other; BB5 needs BB1–BB4; BB6 needs BB5 and the M1b terminal,
which has shipped. Goal: an interactive `ash` on the framebuffer
terminal with `cd`/`ls`/`cat`/pipes/redirection/job control.

### DL. Dynamic linking (deferred)
Spec: `docs/superpowers/specs/2026-09-01-busybox-and-dynamic-linking-roadmap.md`

DL1 file-backed `mmap` (`MAP_PRIVATE` demand-paged through the M1a
`vma_fault` path, `MAP_FIXED` for ld.so segment placement), DL2
`PT_INTERP` + full auxv, DL3 dynamic TLS + `dlopen`. Taken up after
BusyBox.
