# Extended CPU State Milestone

**Date:** 2026-08-27
**Status:** Approved
**Roadmap position:** Milestone 3 of 17, taken after signals
(see `2026-08-27-roadmap-architecture-design.md`)

## Purpose

Replace the fixed 512-byte FXSAVE area with `XSAVE`-based, dynamically
sized per-thread state, and enable AVX, AVX2 and MMX for user programs.

This is the milestone the signals work deferred: the signal frame's FP
area was defined as FXSAVE-shaped with a note that this milestone must
widen it.

## What the current code forces

- **`check_features()` halts the machine** if any required SSE feature
  is missing (`kernel/cpu.c:53`). NeoOS's existing model is "require,
  don't adapt".
- **`default_fpu_state` is a fixed 512-byte static**, and
  `cpu_default_fpu_state` copies exactly `FPU_STATE_SIZE` bytes.
- **`struct thread` carries `uint8_t fpu_state[512]` inline**, so a
  thread's FP area is part of its allocation.
- **`USER_CFLAGS` is `-mno-mmx -msse3 -mssse3 -msse4.1 -msse4.2`.**
- **The signal frame hardcodes `SIGFRAME_FPSTATE_SIZE 512`** and uses
  `fxsave`/`fxrstor`.

## Decisions

| # | Decision | Chosen |
|---|---|---|
| 1 | AVX baseline | Runtime detection; userland baseline stays SSE4.2 |
| 2 | Save format | Standard (uncompacted) `XSAVE`, `XSAVEOPT` when available |
| 3 | Signal frame | Carries the full xstate, Linux-shaped |
| 4 | Save policy | Eager, as today |
| 5 | Per-thread storage | Pointer to a right-sized allocation, not inline |

### On decision 1

Compiling userland with `-mavx2` would make AVX2 a hard requirement for
**every** NeoOS program. `-cpu Nehalem` would stop working entirely and
the project's whole existing test suite would need a new QEMU model.
Instead the kernel enables `XSAVE` for whatever `XCR0` features CPUID
reports, so it is correct on either CPU, and only the AVX test program
is built with `-mavx2` and guards itself.

The kernel-side work is identical either way, because `CPUID.0Dh`
sizing is dynamic regardless.

### On decision 2

The signal frame's FP area is user-visible ABI. Linux uses the
uncompacted layout precisely so handlers and `ucontext` consumers can
parse it. Using a compacted format (`XSAVEC`) in the kernel and an
uncompacted one in the frame would mean two layouts and a conversion
between them; the only thing compaction buys is size.

## Detection, enablement, sizing

All from CPUID:

```
leaf 1      ECX[26] XSAVE   ECX[27] OSXSAVE   ECX[28] AVX
leaf 7.0    EBX[5]  AVX2
leaf 0xD.0  EAX:EDX = XCR0 bits the CPU supports
            EBX     = xstate size for the CURRENT XCR0
leaf 0xD.1  EAX[0]  XSAVEOPT
```

**Enablement order is load-bearing.** `CR4.OSXSAVE` must be set before
`XSETBV` is legal, and `CPUID.0Dh:EBX` only reports the right size
*after* `XCR0` is written — it reflects the currently-enabled set, not
the supported one. `XCR0.AVX` without `XCR0.SSE` `#GP`s, so those bits
go on together or not at all.

```c
uint64_t xcr0 = XCR0_X87 | XCR0_SSE;
if (have_avx) { xcr0 |= XCR0_AVX; }
xsetbv(0, xcr0);
xstate_size = cpuid_0d_ebx();   /* now meaningful */
```

### The FXSAVE fallback, and why it is in the design

NeoOS already requires SSE4.2, which implies Nehalem-class hardware,
and real Nehalem has `XSAVE` — so in principle `XSAVE` could be a hard
requirement.

But **QEMU's `-cpu Nehalem` model may not expose `XSAVE` even though
real Nehalem does**, and the entire existing test suite runs on that
model. Rather than gamble, `cpu_init` keeps an FXSAVE path with a fixed
512-byte size when `CPUID.1:ECX[26]` is clear.

The implementation plan's first step is to probe what `-cpu Nehalem`
actually reports. That answer decides which path the standard boot
exercises — not whether the code is correct.

### Kernel state

`FPU_STATE_SIZE` stops being a compile-time constant, which is what
forces the rest of the milestone:

```c
static uint32_t xstate_size;    /* CPUID.0Dh:EBX, or 512 for FXSAVE */
static uint64_t xstate_mask;    /* the XCR0 actually enabled        */
static enum { SAVE_FXSAVE, SAVE_XSAVE, SAVE_XSAVEOPT } save_mode;
```

`XSAVEOPT` is used when advertised: it skips components unchanged since
the last restore, which matters on every context switch. It requires
the buffer to have been zeroed once and never aliased, since it decides
what to skip by comparing against the buffer's existing contents.

## Per-thread state

```c
struct thread {
    ...
    void *xstate;    /* xstate_size bytes, 64-byte aligned */
};
```

**Allocation is already safe.** The threads milestone made `heap.c`'s
page header 64-byte aligned specifically so every `kmalloc` slot
satisfies `fxsave`'s 16-byte and `XSAVE`'s 64-byte requirement. That
groundwork needs no change here.

Each thread's area is **zeroed at allocation and never shared**, as
`XSAVEOPT` requires. `thread_alloc` gains a failure path: `kmalloc` can
return null, and a thread with no xstate area cannot be scheduled.

### API

Save and restore become real functions rather than inline assembly,
because they need `xstate_mask` and `save_mode`, which are `cpu.c`
file-statics and should stay that way:

```c
void     cpu_state_save(void *buf);      /* xsaveopt / xsave / fxsave */
void     cpu_state_restore(void *buf);   /* xrstor  / fxrstor         */
void     cpu_state_init(void *buf);      /* copy the default template */
uint32_t cpu_state_size(void);
```

`schedule()`'s `fpu_save`/`fpu_restore` calls become `cpu_state_save`/
`cpu_state_restore`. The policy stays **eager**: lazy FPU switching
buys little and has a bad history.

The default template is itself dynamically sized, so `cpu_init`
allocates and captures it — which works because `cpu_init` already runs
after `heap_init` in `kmain`.

### Lifetime

**Three sites must free the area**, and missing any one is exactly the
kind of slow leak the gate catches:

- `thread_join`
- the zombie-reaping loop shared by `wait_for_pid` and `wait4`
- the idle thread's kernel-thread drain

`fork_task` copies `cpu_state_size()` bytes into the child's own
buffer instead of the current field-by-field copy of `FPU_STATE_SIZE`.
`exec_task` resets the buffer to the template rather than calling
`cpu_default_fpu_state`.

### A predictable gate consequence

`struct thread` loses ~500 inline bytes and gains a pointer, moving it
to a smaller heap size class and changing `.bss`. Boot-time
`free_frames` will shift. The gate filters `free_frames` for exactly
this reason, but the delta must still be sanity-checked rather than
waved through.

## Signal frame

`SIGFRAME_FPSTATE_SIZE 512` becomes `cpu_state_size()` at runtime, so
`build_frame` reserves the right amount. The frame grows to roughly
1088 bytes with AVX; user stacks are 16KiB, so that is affordable.

The layout stays Linux's: the legacy 512-byte `_fpstate`, then the
xsave header and component area, plus the `sw_reserved` block Linux
writes at `_fpstate` offset 464 (`FP_XSTATE_MAGIC1`, extended size,
feature mask) and `FP_XSTATE_MAGIC2` at the end.

Nothing in NeoOS reads that magic today and musl does not either. It is
included because the frame is ABI, and the alternative is discovering
later that something which *does* parse it cannot.

### Hardening: `rt_sigreturn` must sanitize the xstate header

**New in this milestone, and not optional.** `XRSTOR` `#GP`s if the
header's `xstate_bv` names features outside `XCR0`, or if the header's
reserved bytes are nonzero. The signal frame is **user-writable
memory** — a program can scribble on it between handler entry and
`sigreturn`.

So `rt_sigreturn` must sanitize the header before restoring, not merely
check alignment and mapping as it does today:

```c
    hdr->xstate_bv &= xstate_mask;   /* drop features we did not enable */
    hdr->xcomp_bv   = 0;             /* uncompacted format              */
    /* reserved[] zeroed                                                */
```

Without this, any user program can `#GP` the kernel from a signal
handler. The existing alignment and range checks stay; this is an
additional layer, and it exists only because this milestone makes the
frame's FP area interpretable rather than opaque bytes.

## Userland

**MMX costs one flag.** Dropping `-mno-mmx` from `USER_CFLAGS` is the
whole change: MMX registers alias the x87 stack, which XSAVE's x87
component already preserves. `EMMS` discipline is userland's problem.

**AVX is opt-in per program.** `USER_CFLAGS` keeps its SSE4.2 baseline;
only `avxtest.c` is built with `-mavx -mavx2`, and it guards itself so
it skips cleanly on a CPU without them rather than executing an illegal
instruction.

No new syscalls, so per `CLAUDE.md` the `docs/stdlib.md` change is to
the existing SSE section: AVX/AVX2/MMX become available, subject to a
runtime check, and the note that AVX-512 remains unsupported stands.

## Verification

Existing convention: in-kernel selftests announcing `passed`/`FAILED`,
a userland test program, headless QEMU under `timeout`, serial log
grepping.

- An in-kernel selftest reporting the detected features, the enabled
  `XCR0`, and `cpu_state_size()`. Cheap, and it makes the CPU model
  visible in every boot log.
- `userland/avxtest.c`: fill all 16 `ymm` registers with a distinctive
  pattern, force many context switches (`yield()` in a loop, plus a
  second thread doing the same with a *different* pattern), then verify
  every register survived. **This is the check that actually exercises
  the milestone.** Implementing the kernel side without it would leave
  AVX state permanently zero, where a broken save/restore is
  indistinguishable from a working one.
- The same shape of test for MMX, proving the x87 component covers the
  aliased registers.
- A run under **both** `-cpu Nehalem` and `-cpu Haswell`. Nehalem
  proves the fallback and the skip path; Haswell proves the AVX path.
  Both are required — that pair is the point of choosing runtime
  detection.
- `sigtest` must still pass unchanged, since the signal frame's FP area
  changed size underneath it.
- The leak gate, comparing `free_frames` at 5 and 10 iterations, since
  this milestone adds a per-thread allocation with three free sites.

## Out of scope

- **AVX-512** and its opmask/ZMM state. Recorded in the roadmap already.
- **`XSAVEC`/`XSAVES`** and the compacted format.
- **Lazy FPU switching.**
- **`XCR0` manipulation from user mode** — there is no `arch_prctl`
  surface for it, and musl does not need one.
