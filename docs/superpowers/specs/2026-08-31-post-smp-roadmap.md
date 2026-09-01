# Post-SMP roadmap — driver and hardening milestones

**Date:** 2026-08-31
**Status:** planning index. Each milestone below has (or gets) its own
design spec + implementation plan under `docs/superpowers/`.

This document sequences the work requested after the SMP
object-lifetime milestone: x2APIC, a floppy (FDC) driver, an audio
stack (SB16 / AC97 / Intel HDA), and ASLR + NX/W^X. It is written to be
executed autonomously in order; each milestone ends gauntlet-green and
is independently shippable.

## Ordering and rationale

```
  in flight ── SMP lifetime + lock detangle   (Tasks 1-8, this session)
      │
      ├─ A. net socket-lifetime fix            (residual from that milestone)
      │
      ├─ B. NX / W^X enforcement               ← DONE (commits 9cabab4..d872e7c)
      ├─ C. ASLR                               ← builds on B and on AT_RANDOM
      │
      ├─ M1a. Console plumbing              ← DONE (commits 627a113..8045377)
      │      /dev/fb0 + fbcon, poll/select, PTY, allocatable struct tty,
      │      devfs_register. plan 2026-09-01-m1a-console-plumbing.md.
      │      poll/select also unblocks TCP.
      ├─ M2. init (PID 1)                   ← DONE. /SBIN/INIT from
      │      /ETC/INITTAB, reboot(2) (PID-1-only), orphan reparenting,
      │      the workload runs through init. spec 2026-09-01-m2-init-design.md,
      │      plan 2026-09-01-m2-init.md.
      ├─ M1b. Framebuffer terminal          ← NEXT: xterm-ish VT + scrollback +
      │      NokiaPure-as-PSF; the kernel out of the printing business.
      │
      ├─ DL.  Dynamic linking               ← after M1b. file-backed mmap,
      │      PT_INTERP + full auxv, dynamic TLS + dlopen. Highest leverage
      │      on "run real Linux apps unpatched".
      ├─ BB.  BusyBox (static)              ← after DL. execve+argv/envp,
      │      job control, minimal /proc, Tier-2 fs syscalls, then an
      │      interactive ash in the M1b terminal.
      │      Both tracks: spec 2026-09-01-busybox-and-dynamic-linking-roadmap.md.
      │
      ├─ D. x2APIC                             ← isolated; unblocks >255 CPUs later
      ├─ E. FDC (floppy) driver                ← isolated; smallest new driver
      │
      └─ F. Audio stack
             F1. sound core + /dev/dsp (OSS-style PCM)
             F2. SB16    (ISA DMA — simplest real codec)
             F3. AC97    (PCI bus-master DMA)
             F4. Intel HDA (PCI, codec/widget graph)
```

**Why this order:**

- **Security first (B, C).** NX/W^X is a page-table audit plus a few
  loader changes — small, and it removes a whole class of exploit
  primitive. ASLR then layers on top and reuses the `AT_RANDOM` entropy
  path that Phase 12 already built. Doing ASLR without W^X first is
  putting a lock on a door with no wall.
- **x2APIC (D) is orthogonal.** It touches only `kernel/dev/lapic.c`,
  `kernel/dev/ioapic.c` and the SMP bring-up. It has no dependency on
  anything else and nothing depends on it in the short term (the
  benefit — >255 logical CPUs, faster IPIs via MSR — is latent). Slot
  it wherever convenient.
- **FDC (E) is the cheap driver win.** A legacy, fully-documented
  device; a self-contained block driver behind the existing VFS/blkdev
  seam. Good warm-up for the audio work and useful for boot-floppy
  images.
- **Audio (F) last and largest.** It needs a new device class (there is
  no PCM/mixer abstraction today), a userland ABI decision (OSS
  `/dev/dsp` vs a cut-down ALSA), and three drivers of increasing
  complexity. SB16 first because ISA DMA is the least machinery; HDA
  last because the codec graph is.

## Cross-cutting constraints (all milestones)

- **No host unit tests.** Every test is a kernel selftest or a userland
  binary, verified by `make test` (headless QEMU, 4 CPUs) and the
  15-run parallel gauntlet
  (`.superpowers/sdd/2026-08-31-phase14-input-and-solidity/pgauntlet.sh`).
- **Lock ranks are enforced.** Any new lock gets a `LOCK_RANK_*` slot
  in `kernel/sync/lock.h` with a written rationale; acquiring out of
  order panics the boot.
- **The ABI is not ours.** Anything a user program can observe --
  `/dev/dsp` ioctls, `mmap` of a DMA buffer, evdev-style event structs,
  auxv entries, signal-frame layout -- matches Linux x86-64 values and
  struct layouts. Syscall *numbers* stay NeoOS's own; the musl shim
  translates. Every deliberate divergence is recorded in
  `docs/stdlib.md`.
- **Every user-facing feature needs a musl path or a `lib/` wrapper**
  plus a `docs/stdlib.md` entry (CLAUDE.md).
- **QEMU is the reference machine.** `-cpu Nehalem -smp 4`. Device
  models: `qemu -device` for AC97 (`AC97`), HDA (`intel-hda` +
  `hda-duplex`), floppy (`-fda` / `-device floppy`), and SB16
  (`-device sb16`, ISA). x2APIC is a CPU feature flag
  (`-cpu ...,+x2apic` or `-machine ...,x2apic=on`).
- **Milestone close:** refresh `docs/abi-compatibility.md` and
  `docs/optimization-summary.md` (or the milestone's own summary),
  update `docs/stdlib.md`.

## Milestone briefs

### A. Net socket-lifetime fix
Spec/plan: folded into `2026-08-31-smp-lifetime-and-lock-detangle` as a
follow-on commit. A blocked reader in `socket.c:recv_one` holds no
reference on its socket, so `sock_close` -> `sock_free` on another
thread can free `s->lock` / `s->readers` out from under
`waitq_sleep`'s re-acquire (seen once as `[lock] PANIC: schedule() with
a spinlock held ... holding=socktable`). Fix: `sock_get`/`sock_put`
reference count; `recv_one` holds a ref across the wait. ~20 lines.

### B. NX / W^X enforcement
Design: `docs/superpowers/specs/2026-08-31-nx-wxorx-design.md`
- Audit every `paging_map_into` / mmap / ELF-load call site for pages
  mapped both writable and executable. Kernel `.text` read-only + `.data`
  NX; user stacks/heap/mmap NX by default; enforce `PROT_EXEC` xor
  `PROT_WRITE` on `mmap`/`mprotect` unless `MAP_` says otherwise
  (matching Linux, which allows W+X but is increasingly locked down --
  NeoOS can be strict and record the divergence).
- EFER.NXE is already set (paging works with `PAGE_NO_EXECUTE`); this
  is mostly making the flag's *use* consistent and adding the
  `mprotect` check.

### C. ASLR
Design: `docs/superpowers/specs/2026-08-31-aslr-design.md`
- Randomize: the mmap base hint, the main thread stack top, the ELF
  load base for `ET_DYN` (PIE) binaries, and the brk start. Entropy
  from the same source `AT_RANDOM` uses (Phase 12). A boot flag / sysctl
  to disable for debugging (Linux `kernel.randomize_va_space`).
- Per-arch entropy bits chosen to match Linux x86-64 defaults (28 bits
  mmap, 32 bits for PIE with `CONFIG_ARCH_MMAP_RND_BITS`-equivalent).

### D. x2APIC
Design: `docs/superpowers/specs/2026-08-31-x2apic-design.md`
- Detect `CPUID.01:ECX.x2APIC`; if present and firmware allows, set
  `IA32_APIC_BASE.EXTD` and switch LAPIC access from MMIO to the
  `IA32_X2APIC_*` MSR range. Fall back to xAPIC otherwise.
- ICR becomes a single 64-bit MSR write (no "wait for delivery" poll).
- APIC IDs widen to 32-bit; `struct cpu` and the id->cpu map adjust.
- IOAPIC RTE programming is unchanged; only the destination field
  widens for logical/physical modes.

### E. FDC (floppy) driver
Design: `docs/superpowers/specs/2026-08-31-fdc-driver-design.md`
- 82077AA-compatible controller at ports 0x3F0-0x3F7, IRQ 6, ISA DMA
  channel 2. Motor control + spin-up delay, seek/recalibrate, read/write
  via the DMA buffer (bounce buffer below 16 MB, like the existing ATA
  path's constraints).
- Exposes a block device through the existing `struct blockdev` seam so
  FAT can mount a floppy image (`mount /dev/fd0 ...`).
- QEMU `-fda image.img`. A selftest that reads a known sector and, on a
  writable image, round-trips one.

### F. Audio stack
Design: `docs/superpowers/specs/2026-08-31-audio-stack-design.md`
covers F1-F4 as one architecture; F2-F4 get their own plans.
- **F1 sound core:** a `struct snd_pcm` device class + `/dev/dsp`
  (OSS ioctl subset: `SNDCTL_DSP_SPEED`, `_SETFMT`, `_CHANNELS`,
  `_GETOSPACE`, `SNDCTL_DSP_SYNC`) and `/dev/mixer` (`SOUND_MIXER_*`).
  OSS chosen over ALSA: far smaller ABI surface, `write()`-driven, and
  what a from-scratch kernel can implement completely rather than
  partially. Divergence from Linux (which is ALSA-native, OSS via
  emulation) recorded in `docs/stdlib.md`.
- Ring-buffer of PCM periods, `poll`-able, `write()` blocks when full.
- **F2 SB16:** ISA DMA (8/16-bit channels), fixed 44.1 kHz path first.
- **F3 AC97:** PCI device, bus-master descriptor list (BDL) of
  buffer-descriptor entries, 48 kHz fixed-rate codec.
- **F4 Intel HDA:** PCI, CORB/RIRB command rings, a stream descriptor
  with its own BDL, codec enumeration to find a line-out DAC + pin.
  The widget graph walk is the bulk of the work.

## Status (2026-08-31, autonomous session)

Specs AND implementation plans are written for every milestone:

| # | Design spec | Implementation plan |
|---|---|---|
| A net socket | (folded — commit `a66e5f7`, DONE) | — |
| B NX/W^X | `2026-08-31-nx-wxorx-design.md` | `plans/2026-08-31-nx-wxorx.md` — **DONE** (commits `9cabab4`..`d872e7c`) |
| C ASLR | `2026-08-31-aslr-design.md` | `plans/2026-08-31-aslr.md` |
| D x2APIC | `2026-08-31-x2apic-design.md` | `plans/2026-08-31-x2apic.md` |
| E FDC | `2026-08-31-fdc-driver-design.md` | `plans/2026-08-31-fdc-driver.md` |
| F audio | `2026-08-31-audio-stack-design.md` | `plans/2026-08-31-audio-p1-core-and-sb16.md` (P1); P2 PCI / P3 AC97 / P4 HDA plans pending P1+PCI |

## Notes for the executor

- Every spec makes calls the user would normally decide (OSS vs ALSA,
  W+X strictness, ASLR entropy bits, x2APIC-always vs opt-in, the
  `struct blockdev` seam); each is stated with rationale and an "alt:".
  **These were not confirmed by the user** — a review pass on the
  specs is the right first step when they return.
- B/C are bounded hardening of existing code; their plans are
  gauntlet-gated per task and safe to execute after a spec review.
- D/E/F are new subsystems with ABI or hardware surface — review the
  spec, then execute the plan.
- The Phase 14 plan (`plans/2026-08-31-phase14-input-and-solidity.md`)
  resumes at Task 2 and carries the two audio prerequisites
  (`file_ops` ioctl/poll, devfs dynamic registration). Do Phase 14
  before audio P1.
- Each plan's Task 1 (or Global Constraints) restates that a single
  `make test` is NOT sufficient sign-off — the parallel gauntlet
  (`pgauntlet.sh`, CONC=3, `PGAUNTLET PASSED: 15/15`) is the bar.
