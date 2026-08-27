# Extended CPU State Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the fixed 512-byte FXSAVE area with `XSAVE`-based, dynamically sized per-thread state, and make AVX, AVX2 and MMX usable by user programs.

**Architecture:** The kernel enables `XCR0` for whatever CPUID reports and sizes every buffer from `CPUID.0Dh:EBX`, so it is correct on a Nehalem or a Haswell. Each thread owns a right-sized, 64-byte-aligned allocation instead of an inline array. The signal frame's FP area grows to match and gains a sanitized xstate header.

**Tech Stack:** C (gnu11, freestanding, `-mcmodel=kernel`), NASM, x86-64, GRUB/Multiboot2, QEMU for verification.

**Spec:** `docs/superpowers/specs/2026-08-27-extended-state-design.md`
**Roadmap:** `docs/superpowers/specs/2026-08-27-roadmap-architecture-design.md`

## Global Constraints

- **No host test runner.** Every verification is an in-kernel selftest or a userland test program under headless QEMU, checked by grepping the serial log. Never write or propose host unit tests.
- **QEMU never exits on its own.** Always wrap it in `timeout`.
- **Every verification needs a fresh disk image.** `fat16_write_selftest` creates `/NEWDIR`, so a stale image reports `FAILED` on the second and later boots. Always `rm -f build/disk.img build/disk2.img` before `make disk-image`. This has produced false alarms four times in this project's history — twice in the last milestone alone.
- **Regenerate the disk images BETWEEN the two CPU models, not just
  before the pair.** The first run creates `/NEWDIR`, so the second
  reports `fat16_write_selftest FAILED` on an otherwise perfect boot.
  Every dual-model loop in this plan needs
  `rm -f build/disk.img build/disk2.img && make disk-image` *inside*
  the loop.
- **Do not put `pkill -f qemu-system-x86_64` in the same shell command
  as anything else** — it matches and kills the invoking shell's own
  process group, aborting the rest of the command with exit 144. Kill
  strays in a separate call, or by pid.
- **Never put `pgrep -f qemu-system` or `pkill -f qemu-system` in a
  command whose own command line contains that string** — the pattern
  matches the invoking shell, so `pkill` kills the command before it
  runs anything (exit 144) and `pgrep` reports a phantom "leftover"
  that is really itself. Match by `comm` instead:
  `ps ax -o pid,comm | grep -i qemu`.
- **Kill stray QEMU processes before a run.** A previous run still holding `build/disk.img` fails the next one with `Failed to get "write" lock`, and the log comes back empty. `pkill -f qemu-system-x86_64` first if in doubt.
- **Work happens directly on `main`.** No feature branches (`CLAUDE.md`).
- **The kernel stays `-mno-sse`.** Only userland gains AVX/MMX. Kernel code must never touch FP registers; `cpu.c`'s inline assembly is the sole exception and it only names the save/restore instructions.
- **Two QEMU models are required this milestone**, not one:

```bash
QEMU_NEHALEM="-cpu Nehalem"     # proves the fallback and the AVX skip path
QEMU_HASWELL="-cpu Haswell"     # proves the AVX path
# common:
COMMON="-boot order=d -cdrom build/neoos.iso \
  -drive file=build/disk.img,format=raw -drive file=build/disk2.img,format=raw \
  -display none -no-reboot"
```

### The "boot log UNCHANGED" check

Task 3 is a refactor that must not change observable behavior. Byte
comparison does not work — two runs of the identical binary differ by
~57 lines because preemption interleaves differently. Filter the
timing-dependent lines and compare the **sorted multiset**:

```bash
filter() {
  grep -vE '^\[timer\] tick=|calibrated lapic|kmain address=|free_frames=|^\[(looper|yielder) pid=[0-9]+\] tick$' "$1" | sort
}
```

Validated across two prior milestones: with this filter two runs of one
binary differ by **0** lines. `free_frames=` is filtered because
changing `.bss` moves it — Task 3 shrinks `struct thread` by ~500
bytes, so a shift is expected. **Check the delta is explainable rather
than ignoring it.**

**One more thing the filter does not remove, found during execution:**
the *pid* of a process forked at run time (`sigtest`'s `check_segv`
child) is timing-dependent. It takes whatever `next_id` holds at the
instant of the fork, and how many threads other processes have created
by then depends on scheduling — so any change to per-thread work shifts
it. When the gate reports a single differing line of that shape, verify
it is only an id by comparing the *multiset* of exit codes, the count
of `task exited` lines, and the sorted test results:

```bash
grep -o 'code=0x[0-9a-f]*' "$1" | sort            # exit-code multiset
grep -c 'task exited' "$1"                        # process count
grep -E "ALL PASSED|passed$|FAILED" "$1" | sort   # results
```

All three identical plus one shifted pid is explainable. Any other
difference is not.

---

## File Structure

**Created:**

| File | Responsibility |
|---|---|
| `userland/avxtest.c` | the milestone's proof: AVX/MMX state across context switches |

**Modified:** `kernel/cpu.h`, `kernel/cpu.c`, `kernel/sched/proc.h`, `kernel/sched/proc.c`, `kernel/sched/thread.c`, `kernel/sched/sched.c`, `kernel/signal.h`, `kernel/signal.c`, `kernel/kernel.c`, `docs/stdlib.md`, `Makefile`.

---

## Task 1: Probe what the QEMU models actually report

**Files:**
- Modify: `kernel/cpu.c` (temporary, reverted)

**Interfaces:**
- Produces: nothing. This is a measurement.

The spec's FXSAVE fallback exists because **QEMU's `-cpu Nehalem` may
not expose `XSAVE` even though real Nehalem does**. Find out before
building anything on the assumption.

- [ ] **Step 1: Add a temporary CPUID dump**

In `kernel/cpu.c`, add a `cpuid_count` helper and call a probe at the
end of `cpu_init`:

```c
static void cpuid_count(uint32_t leaf, uint32_t sub,
                        uint32_t *eax, uint32_t *ebx,
                        uint32_t *ecx, uint32_t *edx) {
    __asm__ volatile ("cpuid"
                      : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                      : "a"(leaf), "c"(sub));
}

static void probe_xsave(void) {
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    serial_write_string("[probe] leaf1.ecx=");   serial_write_hex64(c);
    serial_write_string(" XSAVE=");   serial_write_hex64((c >> 26) & 1);
    serial_write_string(" OSXSAVE="); serial_write_hex64((c >> 27) & 1);
    serial_write_string(" AVX=");     serial_write_hex64((c >> 28) & 1);
    serial_write_string("\n");

    cpuid_count(7, 0, &a, &b, &c, &d);
    serial_write_string("[probe] leaf7.ebx=");  serial_write_hex64(b);
    serial_write_string(" AVX2=");  serial_write_hex64((b >> 5) & 1);
    serial_write_string("\n");

    cpuid_count(0xD, 0, &a, &b, &c, &d);
    serial_write_string("[probe] xcr0_supported=");
    serial_write_hex64(((uint64_t)d << 32) | a);
    serial_write_string(" size_cur="); serial_write_hex64(b);
    serial_write_string(" size_max="); serial_write_hex64(c);
    serial_write_string("\n");

    cpuid_count(0xD, 1, &a, &b, &c, &d);
    serial_write_string("[probe] leaf0d1.eax="); serial_write_hex64(a);
    serial_write_string(" XSAVEOPT="); serial_write_hex64(a & 1);
    serial_write_string("\n");
}
```

- [ ] **Step 2: Run under both models**

```bash
pkill -f qemu-system-x86_64
rm -f build/disk.img build/disk2.img
make clean && make iso disk-image
for CPU in Nehalem Haswell; do
  timeout 60 qemu-system-x86_64 -cpu $CPU -boot order=d -cdrom build/neoos.iso \
    -drive file=build/disk.img,format=raw -drive file=build/disk2.img,format=raw \
    -display none -no-reboot -serial file:/tmp/probe-$CPU.log
  echo "=== $CPU ==="; grep '\[probe\]' /tmp/probe-$CPU.log
done
```

**Record both outputs in the commit message.** They decide which path
the standard boot exercises for the rest of the milestone:

- Nehalem reporting `XSAVE=1` → the standard boot exercises the XSAVE
  path with x87+SSE only, and the FXSAVE fallback is dead code that
  still must compile and stay correct.
- Nehalem reporting `XSAVE=0` → the standard boot exercises the FXSAVE
  fallback, and only the Haswell run covers XSAVE.

Note `size_cur` at this point reflects `XCR0 = 1` (x87 only), because
nothing has written `XCR0` yet — that is expected, and is exactly the
ordering trap the spec warns about.

- [ ] **Step 3: Revert the probe**

Remove `probe_xsave` and its call. Keep `cpuid_count` — Task 2 needs
it.

- [ ] **Step 4: Commit**

```bash
git add kernel/cpu.c
git commit -m "Add cpuid_count helper; record XSAVE availability per QEMU model"
```

---

## Task 2: Detection, `XCR0` enablement, and the save/restore API

**Files:**
- Modify: `kernel/cpu.h`, `kernel/cpu.c`, `kernel/kernel.c`

**Interfaces:**
- Produces: `uint32_t cpu_state_size(void)`; `void cpu_state_save(void *)`; `void cpu_state_restore(void *)`; `void cpu_state_init(void *)`; `int cpu_has_avx(void)`; `void cpu_state_selftest(void)`.
- Consumes: `cpuid_count` from Task 1.

No thread changes yet. `struct thread` keeps its inline array and
`schedule()` keeps calling the old helpers, so this task is additive
and cannot break the boot.

- [ ] **Step 1: Rewrite `kernel/cpu.h`**

```c
#ifndef NEOOS_CPU_H
#define NEOOS_CPU_H

#include <stdint.h>

// Upper bound on the extended state this kernel will ever enable:
// x87 + SSE + AVX. AVX-512 is explicitly out of scope (see the roadmap
// spec), so no configuration can exceed this. Used only to size the
// static default template; per-thread areas are exactly
// cpu_state_size() bytes.
#define CPU_STATE_MAX 1024

// Detects CPU features via CPUID, enables SSE and (where available)
// XSAVE with the widest XCR0 this kernel supports, sizes the extended
// state area, and captures the default template new threads start
// from. Halts with a diagnostic if a required SSE feature is missing --
// the kernel binary is compiled assuming their presence. AVX is
// OPTIONAL and detected, never required.
void cpu_init(void);

// Size in bytes of one thread's extended-state area. Valid only after
// cpu_init(). 512 when the CPU has no XSAVE.
uint32_t cpu_state_size(void);

// 1 if AVX was detected and enabled in XCR0.
int cpu_has_avx(void);
int cpu_has_avx2(void);

// The XCR0 actually enabled. Signal delivery records it in the frame,
// and rt_sigreturn masks a user-supplied xstate header against it.
uint64_t cpu_state_xcr0(void);

// Saves/restores the calling CPU's extended state to/from a
// cpu_state_size()-byte, 64-byte-aligned buffer. Used by schedule()
// around every context switch, and by signal delivery.
void cpu_state_save(void *buf);
void cpu_state_restore(void *buf);

// Initialises `buf` (cpu_state_size() bytes) to the default state a
// freshly created thread starts from.
void cpu_state_init(void *buf);

void cpu_state_selftest(void);

#endif
```

`FPU_STATE_SIZE`, `cpu_default_fpu_state`, `fpu_save` and `fpu_restore`
have callers in `sched.c`, `thread.c`, `proc.c` and `signal.c` that
Tasks 3 and 4 convert. **Keep them as shims for now**, at the bottom of
`cpu.h`, so this task is additive and the tree keeps building and
booting:

```c
// TEMPORARY: deleted by the per-thread-state and signal-frame tasks,
// which convert the last callers. Present only so those conversions can
// be separate, separately-verified changes.
#define FPU_STATE_SIZE 512
static inline void fpu_save(void *b)    { __asm__ volatile ("fxsave (%0)"  :: "r"(b) : "memory"); }
static inline void fpu_restore(void *b) { __asm__ volatile ("fxrstor (%0)" :: "r"(b) : "memory"); }
void cpu_default_fpu_state(void *dest);
```

Leaving them permanently would mean two ways to save FP state, which is
exactly the duplication this milestone removes.

- [ ] **Step 2: Implement detection and enablement in `cpu.c`**

```c
#define CPUID_1_ECX_XSAVE   (1u << 26)
#define CPUID_1_ECX_OSXSAVE (1u << 27)
#define CPUID_1_ECX_AVX     (1u << 28)
#define CPUID_7_EBX_AVX2    (1u << 5)
#define CPUID_D1_EAX_XSAVEOPT (1u << 0)

#define CR4_OSXSAVE (1ULL << 18)

#define XCR0_X87 (1ULL << 0)
#define XCR0_SSE (1ULL << 1)
#define XCR0_AVX (1ULL << 2)

static uint32_t xstate_size = 512;
static uint64_t xstate_mask;
static int      have_avx, have_avx2;
static enum { SAVE_FXSAVE, SAVE_XSAVE, SAVE_XSAVEOPT } save_mode = SAVE_FXSAVE;

static void xsetbv(uint32_t index, uint64_t value) {
    uint32_t lo = (uint32_t)value, hi = (uint32_t)(value >> 32);
    __asm__ volatile ("xsetbv" :: "c"(index), "a"(lo), "d"(hi));
}

static void enable_xsave(void) {
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    if (!(c & CPUID_1_ECX_XSAVE)) {
        serial_write_string("[cpu] no XSAVE -- using FXSAVE, 512-byte state\n");
        return;                       // save_mode stays SAVE_FXSAVE
    }
    have_avx = (c & CPUID_1_ECX_AVX) != 0;

    cpuid_count(7, 0, &a, &b, &c, &d);
    have_avx2 = have_avx && (b & CPUID_7_EBX_AVX2) != 0;

    // ORDER IS LOAD-BEARING. CR4.OSXSAVE must be set before XSETBV is
    // legal, and CPUID.0Dh:EBX only reports the right size AFTER XCR0
    // is written -- it reflects what is ENABLED, not what is supported.
    // Reading it earlier yields a too-small buffer and silent
    // corruption.
    uint64_t cr4 = read_cr4();
    cr4 |= CR4_OSXSAVE;
    write_cr4(cr4);

    // XCR0.AVX without XCR0.SSE #GPs, so those bits go on together or
    // not at all.
    xstate_mask = XCR0_X87 | XCR0_SSE;
    if (have_avx) { xstate_mask |= XCR0_AVX; }
    xsetbv(0, xstate_mask);

    cpuid_count(0xD, 0, &a, &b, &c, &d);
    xstate_size = b;
    if (xstate_size < 512 || xstate_size > CPU_STATE_MAX) {
        serial_write_string("[cpu] implausible xstate size -- falling back to FXSAVE\n");
        xstate_size = 512;
        xstate_mask = 0;
        return;
    }

    cpuid_count(0xD, 1, &a, &b, &c, &d);
    save_mode = (a & CPUID_D1_EAX_XSAVEOPT) ? SAVE_XSAVEOPT : SAVE_XSAVE;
}
```

- [ ] **Step 3: Implement save, restore and the template**

```c
static uint8_t default_state[CPU_STATE_MAX] __attribute__((aligned(64)));

void cpu_state_save(void *buf) {
    uint32_t lo = (uint32_t)xstate_mask, hi = (uint32_t)(xstate_mask >> 32);
    switch (save_mode) {
    case SAVE_XSAVEOPT:
        __asm__ volatile ("xsaveopt (%0)" :: "r"(buf), "a"(lo), "d"(hi) : "memory");
        break;
    case SAVE_XSAVE:
        __asm__ volatile ("xsave (%0)" :: "r"(buf), "a"(lo), "d"(hi) : "memory");
        break;
    default:
        __asm__ volatile ("fxsave (%0)" :: "r"(buf) : "memory");
        break;
    }
}

void cpu_state_restore(void *buf) {
    uint32_t lo = (uint32_t)xstate_mask, hi = (uint32_t)(xstate_mask >> 32);
    if (save_mode == SAVE_FXSAVE) {
        __asm__ volatile ("fxrstor (%0)" :: "r"(buf) : "memory");
    } else {
        __asm__ volatile ("xrstor (%0)" :: "r"(buf), "a"(lo), "d"(hi) : "memory");
    }
}

uint32_t cpu_state_size(void) { return xstate_size; }
uint64_t cpu_state_xcr0(void) { return xstate_mask; }
int cpu_has_avx(void)  { return have_avx; }
int cpu_has_avx2(void) { return have_avx2; }

void cpu_state_init(void *buf) {
    uint8_t *out = (uint8_t *)buf;
    for (uint32_t i = 0; i < xstate_size; i++) { out[i] = default_state[i]; }
}

static void capture_default_state(void) {
    // XSAVEOPT decides what to skip by comparing against the buffer's
    // existing contents, so every area it will ever write must start
    // zeroed. That includes this template, which is copied into every
    // new thread's area.
    for (uint32_t i = 0; i < CPU_STATE_MAX; i++) { default_state[i] = 0; }

    __asm__ volatile ("fninit");
    uint32_t mxcsr_default = 0x1F80;
    __asm__ volatile ("ldmxcsr %0" :: "m"(mxcsr_default));
    cpu_state_save(default_state);
}
```

`cpu_init` becomes `check_features(); enable_sse(); enable_xsave();
capture_default_state();` plus a summary line:

```c
    serial_write_string("[cpu] state: size=");
    serial_write_hex64(xstate_size);
    serial_write_string(" xcr0=");
    serial_write_hex64(xstate_mask);
    serial_write_string(save_mode == SAVE_XSAVEOPT ? " mode=xsaveopt"
                      : save_mode == SAVE_XSAVE    ? " mode=xsave"
                                                   : " mode=fxsave");
    serial_write_string(have_avx2 ? " avx2\n" : have_avx ? " avx\n" : "\n");
```

- [ ] **Step 4: Add the selftest**

```c
void cpu_state_selftest(void) {
    if (xstate_size < 512) {
        serial_write_string("[cpu] selftest FAILED: state size below 512\n");
        return;
    }
    if (save_mode != SAVE_FXSAVE && (xstate_mask & (XCR0_X87 | XCR0_SSE))
                                     != (XCR0_X87 | XCR0_SSE)) {
        serial_write_string("[cpu] selftest FAILED: XCR0 missing x87/SSE\n");
        return;
    }
    if (have_avx && !(xstate_mask & XCR0_AVX)) {
        serial_write_string("[cpu] selftest FAILED: AVX detected but not enabled\n");
        return;
    }
    if (have_avx && xstate_size <= 512) {
        serial_write_string("[cpu] selftest FAILED: AVX enabled but state still 512\n");
        return;
    }

    // Save/restore round trip through a scratch buffer: the state after
    // a restore must save back byte-identical.
    static uint8_t a[CPU_STATE_MAX] __attribute__((aligned(64)));
    static uint8_t b[CPU_STATE_MAX] __attribute__((aligned(64)));
    for (uint32_t i = 0; i < CPU_STATE_MAX; i++) { a[i] = 0; b[i] = 0; }

    cpu_state_save(a);
    cpu_state_restore(a);
    cpu_state_save(b);
    for (uint32_t i = 0; i < xstate_size; i++) {
        if (a[i] != b[i]) {
            serial_write_string("[cpu] selftest FAILED: save/restore not stable\n");
            return;
        }
    }
    serial_write_string("[cpu] state selftest passed\n");
}
```

Call `cpu_state_selftest();` from `kmain` immediately after
`cpu_init();`.

**Note on the round trip:** with `XSAVEOPT` this can legitimately
differ in the xstate header's `xstate_bv` if a component was skipped.
If the selftest fails only on that field, compare the legacy 512 bytes
and the header's `xstate_bv & xstate_mask` rather than raw bytes — and
record that as a plan correction.

- [ ] **Step 5: Build and verify under both models**

```bash
pkill -f qemu-system-x86_64
rm -f build/disk.img build/disk2.img
make clean && make iso disk-image
for CPU in Nehalem Haswell; do
  timeout 90 qemu-system-x86_64 -cpu $CPU -boot order=d -cdrom build/neoos.iso \
    -drive file=build/disk.img,format=raw -drive file=build/disk2.img,format=raw \
    -display none -no-reboot -serial file:/tmp/t2-$CPU.log
  echo "=== $CPU ==="
  grep -E '\[cpu\]' /tmp/t2-$CPU.log
  grep -c "FAILED\|exception" /tmp/t2-$CPU.log
done
```

Expected: `[cpu] state selftest passed` on both. Haswell must report
`avx2` and a size above 512; Nehalem reports whatever Task 1 found.
Zero `FAILED`/exceptions on both, and the rest of the boot unchanged.

- [ ] **Step 6: Commit**

```bash
git add kernel/cpu.h kernel/cpu.c kernel/kernel.c
git commit -m "Detect and enable XSAVE with a dynamically sized state area"
```

---

## Task 3: Per-thread extended state

**Files:**
- Modify: `kernel/cpu.h`, `kernel/sched/proc.h`, `kernel/sched/proc.c`, `kernel/sched/thread.c`, `kernel/sched/sched.c`

**Interfaces:**
- Consumes: Task 2's `cpu_state_*` API.
- Produces: `struct thread`'s `void *xstate`.

Behavior must not change. Gated on the boot log — **capture the
baseline first**.

- [ ] **Step 1: Capture the baseline** (see Global Constraints), under
`-cpu Nehalem`.

- [ ] **Step 2: Replace the inline array**

In `kernel/sched/proc.h`, replace

```c
    uint8_t fpu_state[FPU_STATE_SIZE] __attribute__((aligned(16)));
```

with

```c
    // cpu_state_size() bytes, 64-byte aligned. A pointer rather than an
    // inline array because the size is only known at run time -- see
    // the extended-state design spec.
    void *xstate;
```

- [ ] **Step 3: Allocate and free it**

In `kernel/sched/thread.c`'s `thread_alloc`, replace
`cpu_default_fpu_state(t->fpu_state);` with:

```c
    // heap.c's page header is 64-byte aligned specifically so every
    // kmalloc slot satisfies fxsave's 16-byte and XSAVE's 64-byte
    // requirement; see the comment there.
    t->xstate = kmalloc(cpu_state_size());
    if (!t->xstate) { kfree(t); return 0; }
    cpu_state_init(t->xstate);
```

and note that the existing comment above `thread_alloc` about
`fxsave`/`fxrstor` alignment now refers to `t->xstate`.

**Three sites free it**, and missing any one is a slow leak the gate
catches. Add `kfree(z->xstate);` immediately before each `kfree(z);`:

- `kernel/sched/sched.c:82` — the idle thread's kernel-thread drain
- `kernel/sched/proc.c:584` — `proc_reap`'s zombie loop
- `kernel/sched/thread.c:209` — `thread_join`

- [ ] **Step 4: Convert the context switch**

In `kernel/sched/sched.c`:

```c
    if (prev) {
        cpu_state_save(prev->xstate);
    }
    cpu_state_restore(next->xstate);
```

- [ ] **Step 5: Convert fork and exec**

In `kernel/sched/proc.c`'s `fork_task`, replace the byte loop:

```c
    for (uint32_t i = 0; i < cpu_state_size(); i++) {
        ((uint8_t *)child->xstate)[i] = ((uint8_t *)current_thread()->xstate)[i];
    }
```

In `exec_task`, replace `cpu_default_fpu_state(current_thread()->fpu_state);`
with `cpu_state_init(current_thread()->xstate);` — a new program starts
with clean FP state either way.

- [ ] **Step 6: Drop the shims this task can drop**

Remove `FPU_STATE_SIZE` and `cpu_default_fpu_state` from `cpu.h`, and
`cpu_default_fpu_state`'s definition from `cpu.c` — this task converted
their last callers.

`fpu_save`/`fpu_restore` must stay until Task 4, which converts
`signal.c`'s two calls. Delete them there.

- [ ] **Step 7: Build and verify the log is unchanged**

Run the gate under `-cpu Nehalem`. Expected: `IDENTICAL`.

`free_frames` is filtered, but check the delta: `struct thread` loses
~500 inline bytes and gains an 8-byte pointer, moving it to a smaller
heap size class, while each thread now takes a separate
`cpu_state_size()` allocation. A shift of a few frames either way is
explainable; a large or growing one is not.

- [ ] **Step 8: Commit**

```bash
git add kernel/cpu.h kernel/sched/proc.h kernel/sched/proc.c \
        kernel/sched/thread.c kernel/sched/sched.c
git commit -m "Give each thread a right-sized extended-state area"
```

---

## Task 4: Signal frame

**Files:**
- Modify: `kernel/signal.h`, `kernel/signal.c`

**Interfaces:**
- Consumes: `cpu_state_size`, `cpu_state_save`, `cpu_state_restore`.

- [ ] **Step 1: Make the frame's FP area dynamically sized**

In `kernel/signal.h`, replace the fixed constants:

```c
// The FP area is allocated SEPARATELY from the frame and reached
// through uc.uc_mcontext.fpstate. It cannot be a member: the frame's
// own address must be 8 mod 16 so the handler sees rsp % 16 == 8 as if
// reached by `call`, which would put every 64-aligned member at the
// wrong offset and make xsave #GP.
#define SIGFRAME_FPSTATE_ALIGN 64

// Linux's software-reserved block, at offset 464 of the legacy
// _fpstate. Nothing in NeoOS reads it and musl does not either; it is
// written because the frame is ABI.
#define FP_XSTATE_MAGIC1 0x46505853u
#define FP_XSTATE_MAGIC2 0x46505845u

struct fpx_sw_bytes {
    uint32_t magic1;
    uint32_t extended_size;   /* legacy 512 + xstate + 4 for magic2 */
    uint64_t xfeatures;
    uint32_t xstate_size;
    uint32_t padding[7];
};

// Sits at offset 512 of the FP area, immediately after the legacy
// FXSAVE region.
struct xstate_header {
    uint64_t xstate_bv;
    uint64_t xcomp_bv;
    uint64_t reserved[6];
};
```

- [ ] **Step 2: Size and populate the area in `build_frame`**

Replace the fixed reservation:

```c
    uint32_t fpsize = cpu_state_size();
    sp -= fpsize + 4;                 /* +4 for the trailing magic2 */
    sp &= ~(uint64_t)(SIGFRAME_FPSTATE_ALIGN - 1);
    uint64_t fpaddr = sp;
```

and the population:

```c
    fr->uc.uc_mcontext.fpstate = fpaddr;
    cpu_state_save((void *)(uintptr_t)fpaddr);

    // Linux-shaped software-reserved block, so anything that parses
    // the frame finds what it expects.
    struct fpx_sw_bytes *sw =
        (struct fpx_sw_bytes *)(uintptr_t)(fpaddr + 464);
    sw->magic1        = FP_XSTATE_MAGIC1;
    sw->xstate_size   = fpsize;
    sw->extended_size = fpsize + 4;
    sw->xfeatures     = cpu_state_xcr0();
    *(uint32_t *)(uintptr_t)(fpaddr + fpsize) = FP_XSTATE_MAGIC2;
```

The zeroing loop and the `user_range_writable` call must both cover
`fpsize + 4`, not 512.

- [ ] **Step 3: Sanitize the header in `rt_sigreturn`**

**This is the security-relevant part of the milestone.** `XRSTOR`
`#GP`s if the header's `xstate_bv` names features outside `XCR0`, or if
its reserved bytes are nonzero — and the frame is user-writable memory
a program can scribble on between handler entry and `sigreturn`.
Without this, any user program can `#GP` the kernel from a handler.

Replace the current restore:

```c
    if (sc->fpstate &&
        (sc->fpstate & (SIGFRAME_FPSTATE_ALIGN - 1)) == 0 &&
        user_range_writable(sc->fpstate, cpu_state_size())) {

        // The frame is USER memory. XRSTOR faults on a header naming
        // features outside XCR0 or with nonzero reserved bytes, so the
        // header is masked to what this kernel enabled before it is
        // ever fed to the CPU.
        if (cpu_state_size() > 512) {
            struct xstate_header *hdr =
                (struct xstate_header *)(uintptr_t)(sc->fpstate + 512);
            hdr->xstate_bv &= cpu_state_xcr0();
            hdr->xcomp_bv   = 0;          /* uncompacted format only */
            for (int i = 0; i < 6; i++) { hdr->reserved[i] = 0; }
        }
        cpu_state_restore((void *)(uintptr_t)sc->fpstate);
    }
```

- [ ] **Step 3b: Delete the last shims**

`signal.c` was the last caller of `fpu_save`/`fpu_restore`. Remove both
from `cpu.h` now.

- [ ] **Step 4: Build and verify under both models**

```bash
pkill -f qemu-system-x86_64
rm -f build/disk.img build/disk2.img
make clean && make iso disk-image
for CPU in Nehalem Haswell; do
  timeout 150 qemu-system-x86_64 -cpu $CPU -boot order=d -cdrom build/neoos.iso \
    -drive file=build/disk.img,format=raw -drive file=build/disk2.img,format=raw \
    -display none -no-reboot -serial file:/tmp/t4-$CPU.log
  echo "=== $CPU ==="
  grep -n "sigtest" /tmp/t4-$CPU.log
  grep -c "FAILED\|exception" /tmp/t4-$CPU.log
done
```

Expected: all nine `sigtest` checks pass on **both** models, unchanged
from the signals milestone. The frame's FP area changed size
underneath them, so this is the check that the change was transparent.

A `#GP` inside `sigreturn` on Haswell but not Nehalem points at the
xstate header: either the size, the alignment, or the sanitization.

- [ ] **Step 5: Commit**

```bash
git add kernel/signal.h kernel/signal.c kernel/cpu.h
git commit -m "Widen the signal frame to the full xstate and sanitize it on sigreturn"
```

---

## Task 5: Userland — MMX, AVX, and the proof program

**Files:**
- Create: `userland/avxtest.c`
- Modify: `Makefile`, `kernel/kernel.c`

**Interfaces:**
- Consumes: everything.

- [ ] **Step 1: Enable MMX for userland**

In the `Makefile`, drop `-mno-mmx` from `USER_CFLAGS`. That is the
entire MMX change: MMX registers alias the x87 stack, which the x87
component already preserves.

Leave the kernel's `CFLAGS` alone — it keeps `-mno-mmx -mno-sse
-mno-sse2`, and must.

- [ ] **Step 2: Write `userland/avxtest.c`**

```c
#include <unistd.h>
#include <stdio.h>
#include <thread.h>
#include <stdint.h>

// Built with -mavx -mavx2, unlike every other program here: the
// userland baseline stays SSE4.2 so NeoOS keeps running on a CPU
// without AVX. This program therefore has to check at RUN time before
// executing a single AVX instruction.
static int have_avx2(void) {
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                              : "a"(1), "c"(0));
    if (!(ecx & (1u << 28))) { return 0; }          // AVX
    if (!(ecx & (1u << 27))) { return 0; }          // OSXSAVE
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                              : "a"(7), "c"(0));
    return (ebx & (1u << 5)) != 0;                  // AVX2
}

// Fills all 16 ymm registers from `seed`, yields many times so the
// scheduler is guaranteed to save and restore them, then checks every
// one survived. Returns 1 on success.
__attribute__((target("avx2")))
static int hammer_ymm(unsigned seed, int rounds) {
    uint32_t pattern[8 * 16];
    for (int r = 0; r < 16; r++) {
        for (int i = 0; i < 8; i++) { pattern[r * 8 + i] = seed + r * 8 + i; }
    }

    __asm__ volatile (
        "vmovdqu   0(%0), %%ymm0\n\tvmovdqu  32(%0), %%ymm1\n\t"
        "vmovdqu  64(%0), %%ymm2\n\tvmovdqu  96(%0), %%ymm3\n\t"
        "vmovdqu 128(%0), %%ymm4\n\tvmovdqu 160(%0), %%ymm5\n\t"
        "vmovdqu 192(%0), %%ymm6\n\tvmovdqu 224(%0), %%ymm7\n\t"
        "vmovdqu 256(%0), %%ymm8\n\tvmovdqu 288(%0), %%ymm9\n\t"
        "vmovdqu 320(%0), %%ymm10\n\tvmovdqu 352(%0), %%ymm11\n\t"
        "vmovdqu 384(%0), %%ymm12\n\tvmovdqu 416(%0), %%ymm13\n\t"
        "vmovdqu 448(%0), %%ymm14\n\tvmovdqu 480(%0), %%ymm15\n\t"
        :: "r"(pattern) : "memory");

    for (int i = 0; i < rounds; i++) { yield(); }

    uint32_t out[8 * 16];
    __asm__ volatile (
        "vmovdqu %%ymm0,   0(%0)\n\tvmovdqu %%ymm1,  32(%0)\n\t"
        "vmovdqu %%ymm2,  64(%0)\n\tvmovdqu %%ymm3,  96(%0)\n\t"
        "vmovdqu %%ymm4, 128(%0)\n\tvmovdqu %%ymm5, 160(%0)\n\t"
        "vmovdqu %%ymm6, 192(%0)\n\tvmovdqu %%ymm7, 224(%0)\n\t"
        "vmovdqu %%ymm8, 256(%0)\n\tvmovdqu %%ymm9, 288(%0)\n\t"
        "vmovdqu %%ymm10,320(%0)\n\tvmovdqu %%ymm11,352(%0)\n\t"
        "vmovdqu %%ymm12,384(%0)\n\tvmovdqu %%ymm13,416(%0)\n\t"
        "vmovdqu %%ymm14,448(%0)\n\tvmovdqu %%ymm15,480(%0)\n\t"
        :: "r"(out) : "memory");

    for (int i = 0; i < 8 * 16; i++) {
        if (out[i] != pattern[i]) {
            printf("[avxtest] FAILED: ymm word %d = %u, want %u\n",
                   i, out[i], pattern[i]);
            return 0;
        }
    }
    return 1;
}

static volatile int worker_ok;
static volatile int worker_done;

// A second thread hammering a DIFFERENT pattern. Without this the test
// would pass even if the kernel restored one global copy of the
// registers to everybody.
static void worker(void *arg) {
    (void)arg;
    worker_ok = hammer_ymm(0xB0000000u, 300);
    worker_done = 1;
    thread_exit(0);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    if (!have_avx2()) {
        printf("[avxtest] SKIPPED: no AVX2 on this CPU\n");
        printf("[avxtest] ALL PASSED\n");
        return 0;
    }

    thread_t t;
    worker_ok = 0; worker_done = 0;
    if (thread_create(&t, worker, 0) != 0) {
        printf("[avxtest] FAILED: thread_create\n");
        return 1;
    }

    int main_ok = hammer_ymm(0xA0000000u, 300);
    while (!worker_done) { yield(); }
    thread_join(t, 0);

    if (!main_ok)   { printf("[avxtest] FAILED: main thread ymm\n");   return 1; }
    if (!worker_ok) { printf("[avxtest] FAILED: worker thread ymm\n"); return 1; }
    printf("[avxtest] ymm preserved across context switches\n");

    // MMX rides on the x87 component, so this proves that too.
    uint64_t mmx_in = 0x0123456789ABCDEFULL, mmx_out = 0;
    __asm__ volatile ("movq %0, %%mm0" :: "m"(mmx_in));
    for (int i = 0; i < 200; i++) { yield(); }
    __asm__ volatile ("movq %%mm0, %0" : "=m"(mmx_out));
    __asm__ volatile ("emms");
    if (mmx_out != mmx_in) {
        printf("[avxtest] FAILED: mm0 not preserved\n");
        return 1;
    }
    printf("[avxtest] mm0 preserved across context switches\n");

    printf("[avxtest] ALL PASSED\n");
    return 0;
}
```

- [ ] **Step 3: Add the Makefile rule and disk entry**

```makefile
$(USERLAND_BUILD)/AVXTEST.ELF: $(USERLAND_DIR)/avxtest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -mavx -mavx2 -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/avxtest.c -L$(LIB_BUILD) -lneoos
```

`-mavx -mavx2` appear **only here**. Add `$(USERLAND_BUILD)/AVXTEST.ELF`
to the `$(DISK_IMG)` prerequisites and an `mcopy` line beside the
others, then `spawn("/BIN/AVXTEST.ELF");` in `kmain`.

The name is 8.3-safe. A longer one would be mangled by `mcopy`, which
has already cost this project a debugging session.

- [ ] **Step 3b: The test must not yield through a function call**

**Correction found during execution — the kernel was innocent both
times.** Two separate defects in the test, each of which looked exactly
like a kernel that drops the AVX component:

1. **`vzeroupper`.** GCC inserts it before a call from AVX code, to
   avoid AVX-SSE transition penalties, and it zeroes the upper 128 bits
   of every `ymm` register. Calling `lib`'s `yield()` inside the
   measurement window therefore destroys the state being measured.
   Symptom: `ymm` words 0-3 preserved, word 4 onward zero. Yield
   through an inline `syscall` instead, and confirm with
   `objdump -d | grep vzeroupper` that none appears between the load
   and the readback.

2. **`rax` declared input-only on that inline `syscall`.** `syscall`
   returns its result in `rax`, so GCC hoisted `mov $2,%eax` out of the
   loop; every iteration after the first ran with the *previous
   return value* — 0, which is `SYS_EXIT`. Symptom: the process exits
   silently with the loop counter as its exit code. `rax` must be an
   output as well as an input:

```c
    long ret;
    __asm__ volatile ("syscall" : "=a"(ret) : "a"((long)SYS_YIELD)
                                : "rcx", "r11", "memory");
```

**Debug this class of failure with a shrunk reproduction**, not the
full boot: spawn only `AVXTEST` and drop the yield counts to ~20, which
turns a 150-second cycle into 30 seconds.

- [ ] **Step 4: Build and verify under both models**

Same dual-model loop. Expected:

- **Haswell**: `ymm preserved across context switches`,
  `mm0 preserved across context switches`, `[avxtest] ALL PASSED`.
- **Nehalem**: `[avxtest] SKIPPED: no AVX2 on this CPU` followed by
  `ALL PASSED` — the guard working, not a failure.

Zero `FAILED`/exceptions on both.

**If Haswell reports a ymm mismatch**, the save/restore is dropping the
AVX component: check that `XCR0.AVX` is set (the Task 2 summary line
reports it) and that `xstate_size` exceeds 512.

- [ ] **Step 5: Commit**

```bash
git add Makefile userland/avxtest.c kernel/kernel.c
git commit -m "Enable MMX for userland and add avxtest proving ymm survives context switches"
```

---

## Task 6: Documentation

**Files:**
- Modify: `docs/stdlib.md`

No new syscalls, so per `CLAUDE.md` the change is to the existing SSE
section rather than a new one.

- [ ] **Step 1: Rewrite the SSE section**

Replace the `## SSE/SSE2/SSE3/SSE4` section with one covering the new
state of affairs:

- SSE through SSE4.2 are guaranteed: the kernel halts at boot without
  them, and every program is compiled for them.
- **MMX is available**, and its registers are preserved across context
  switches because they alias the x87 stack. `EMMS` discipline is the
  program's responsibility.
- **AVX and AVX2 are available only where the CPU has them**, and are
  **not** part of the default compile flags. A program wanting them
  must be built with `-mavx -mavx2` and must check at run time via
  `CPUID` before executing a single AVX instruction, because NeoOS
  still runs on CPUs without it. `userland/avxtest.c` is the worked
  example.
- All of it is saved and restored per thread automatically; a signal
  handler sees its own state and `sigreturn` restores the interrupted
  one.
- **AVX-512 remains unsupported.**

- [ ] **Step 2: Commit**

```bash
git add docs/stdlib.md
git commit -m "Document MMX and opt-in AVX/AVX2 for user programs"
```

---

## Task 7: Leak gate and final regression

**Files:**
- Modify: `kernel/kernel.c` (temporary, reverted)

This milestone adds a per-thread allocation with three free sites, so
the gate matters more than usual.

- [ ] **Step 1: Add the temporary leak-gate thread**

```c
static void xstate_leak_test(void) {
    serial_write_string("[test] before: free_frames=");
    serial_write_hex64(pmm_free_frame_count());
    serial_write_string(" vnodes=");
    serial_write_hex64(vfs_vnode_in_use_count());
    serial_write_string("\n");

    for (int i = 0; i < 5; i++) {
        struct process *p = spawn("/BIN/AVXTEST.ELF");
        if (!p) {
            serial_write_string("[test] spawn FAILED\n");
        } else {
            wait_for_pid(p->pid);
        }
    }

    serial_write_string("[test] after: free_frames=");
    serial_write_hex64(pmm_free_frame_count());
    serial_write_string(" vnodes=");
    serial_write_hex64(vfs_vnode_in_use_count());
    serial_write_string("\n");

    thread_exit_self(0);
}
```

Temporarily replace `kmain`'s `spawn(...)` calls with
`thread_alloc_kernel(xstate_leak_test);`.

**`wait_for_pid` is the right call here and must stay
parentage-free** — the signals milestone found that routing it through
`wait4` makes a kernel-thread gate spawn everything concurrently and
measure nothing.

- [ ] **Step 2: Run at 5, then at 10**

**Do not accept a nonzero delta as one-time without checking.** The
threads milestone showed 7 frames at 5 iterations, which looked like a
fixed cost and was in fact ~1 frame leaked per cycle; only the
10-iteration run revealed it.

The delta must be **identical** at 5 and 10. `avxtest` creates one
extra thread per run, so a per-thread `xstate` that is allocated but
never freed shows up as growth here — that is precisely what this gate
is for. If it grows, check all three free sites from Task 3 Step 3.

Run under `-cpu Haswell`, where the state is largest.

- [ ] **Step 3: Revert and run the final regression**

Restore `kmain`'s spawns. Confirm `git diff --stat kernel/kernel.c`
shows only the `AVXTEST` line against Task 5.

Run the **full** boot under both models and check every criterion:

- `[cpu] state selftest passed`, and the `[cpu] state: size=… xcr0=…
  mode=…` line reporting the expected values for that model.
- Every prior selftest: `pmm`, `paging`, `lock`, `heap`, `fat16`,
  `fat16 write`, `vfs`, `waitq`, `signal`.
- `[avxtest] ALL PASSED` — with the ymm and mm0 lines on Haswell, and
  the SKIPPED line on Nehalem.
- `[sigtest] ALL PASSED` with all nine checks — the signal frame's FP
  area changed size, so this is the transparency check.
- `[threadtest] ALL PASSED`, `[vfstest] ALL PASSED`,
  `[parent] child exit code=42`.
- `faulter` dying alone, and **zero `exception` lines**.
- Zero `FAILED`.

- [ ] **Step 4: Commit (only if the gate caught something)**

Nothing to commit if the gate passed and `kernel.c` is reverted.

---

## Notes for the implementer

- **The enablement order in Task 2 is the trap.** `CR4.OSXSAVE` before
  `XSETBV`; `CPUID.0Dh:EBX` only after `XCR0` is written. Reading the
  size early gives a too-small buffer, and the corruption is silent —
  it looks like occasional FP garbage in whichever thread was unlucky.
- **`XSAVEOPT` needs zeroed buffers.** It decides what to skip by
  comparing against what the buffer already holds, so an area that was
  never zeroed, or that is shared between threads, produces stale
  state that is very hard to attribute.
- **`XRSTOR` faults on a bad header**, and the signal frame is user
  memory. The sanitization in Task 4 Step 3 is not optional hardening;
  without it a user program crashes the kernel.
- **Run both CPU models every time.** A change that works on Haswell
  and breaks the FXSAVE path on Nehalem — or vice versa — is invisible
  if only one is tested, and the two paths diverge in `cpu.c`,
  `signal.c` and `avxtest.c`.
- **Every verification needs a fresh disk image**, and **kill stray
  QEMU processes first**. Both have already cost time in this project.
- `kernel/cpu.c` will be around 300 lines after this. That is fine.
