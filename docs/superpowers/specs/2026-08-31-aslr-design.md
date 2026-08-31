# ASLR — design

**Date:** 2026-08-31
**Status:** design (solo brainstorm — see roadmap). Depends on the
NX/W^X milestone landing first, and on a real entropy source (which
this spec also introduces, because NeoOS has none).
**Context:** `kernel/sched/proc.c` `build_initial_stack` (AT_RANDOM),
`kernel/mm/vma.c` (`p->mmap_next`), `kernel/mm/vma.h` (`MMAP_BASE`),
`kernel/sched/proc.h` (`USER_STACK_TOP`), `kernel/elf.c`.

## Problem

Every address in a NeoOS process is fixed at link/spawn time:

- ELF load base is whatever `p_vaddr` says (all NeoOS binaries are
  `ET_EXEC`, non-PIE, linked at a fixed address).
- Main-thread stack top is the constant `USER_STACK_TOP`
  (`0x0000700000000000`).
- `mmap` regions bump linearly from the constant `MMAP_BASE`
  (`0x0000500000000000`), fully predictable.
- `brk` starts right after the BSS.
- `AT_RANDOM`'s sixteen bytes are an LCG of the tick counter and a
  couple of addresses — not random, and the only "entropy" in the
  system.

An attacker with any memory-disclosure or overflow primitive knows the
entire address space layout in advance.

## Goals

1. A **software entropy pool** (`kernel/dev/random.c`): mixes RDTSC
   deltas sampled at each timer tick and at interrupt entry into a
   256-bit pool with a simple mixing function; `random_bytes(void*,
   size_t)` drains it through a stream generator, reseeding from the
   pool. Not certified crypto, but unpredictable to a local attacker
   and reseeded continuously. Exposed later as `/dev/urandom` and used
   for `getrandom()`; here it just backs ASLR and `AT_RANDOM`.
   - Optional fast path: if `CPUID` reports RDSEED/RDRAND (it will once
     the QEMU CPU model is bumped past Nehalem — a separate decision),
     fold their output into the pool too. Design does not depend on it.
2. **Randomize per process**, from that pool:
   - `mmap` base: `MMAP_BASE + (rnd & MMAP_RND_MASK)`, page-aligned.
     28 bits of randomness (Linux x86-64 default
     `ARCH_MMAP_RND_BITS`), i.e. up to ~1 GiB of slide.
   - Main stack top: `USER_STACK_TOP - (rnd & STACK_RND_MASK)`,
     16-byte aligned, ~22 bits (Linux uses `0x3ffff << PAGE_SHIFT`).
   - `brk` start: `bss_end + (rnd & BRK_RND_MASK)`, ~13 bits, page
     aligned (Linux `randomize_page`).
   - ELF load base for `ET_DYN` binaries: `ELF_ET_DYN_BASE +
     (rnd & MMAP_RND_MASK)`. Requires PIE support (below).
   - `AT_RANDOM`'s 16 bytes: straight from `random_bytes()`.
3. **A disable knob**: kernel command-line `norandmaps` (and/or a
   `personality(ADDR_NO_RANDOMIZE)` bit), matching Linux
   `kernel.randomize_va_space=0`. Default: on. Tests that assert exact
   addresses run with it off.

## Non-goals

- Making NeoOS's own userland binaries PIE. PIE support in the loader
  is implemented (it is needed to run distro `ET_DYN` executables and
  the dynamic loader later), but rebuilding `userland/*.ELF` as PIE is
  a separate call — they stay `ET_EXEC` and simply are not
  base-randomized, exactly as on Linux for non-PIE binaries.
- Certified CSPRNG, entropy accounting, `getrandom(GRND_RANDOM)`
  blocking semantics. The pool is "good enough for ASLR + stack
  canaries"; a real RNG milestone can replace the core.
- KASLR (randomizing the *kernel* base). Bigger, separate, and lower
  value while the kernel is a single static image.
- Randomizing thread (non-main) stacks beyond the existing slot
  layout, or the vDSO (there is no vDSO yet).

## Design

### 1. Entropy pool — `kernel/dev/random.c`

```c
void   random_init(void);                 // called early in kmain
void   random_add_sample(uint64_t x);     // from timer tick / IRQ entry
void   random_bytes(void *buf, size_t n); // drains, reseeds
uint64_t random_u64(void);
```

- Pool: `uint64_t pool[4]`. `random_add_sample` folds `x` (a RDTSC
  value or delta) in with rotate-xor-multiply (splitmix64-style
  mixing), advancing a write index.
- `random_bytes`: runs a keyed stream (ChaCha8 block function, or a
  simpler xoshiro256** seeded from the pool if ChaCha is judged too
  much) to fill `buf`, then folds one fresh RDTSC back into the pool.
  Lock: `random_lock`, a leaf spinlock, rank `LOCK_RANK_SERIAL`-ish
  (take from IRQ context in `random_add_sample`, hold nothing else).
- Hooks: one line in `waitq_timeout_tick` / the timer ISR to call
  `random_add_sample(rdtsc())`; one line at the top of the generic ISR
  dispatch. Cheap.
- Boot warm-up: `random_init` seeds from a burst of ~64 RDTSC reads
  spaced by `pause`, so the very first `spawn` already has usable
  entropy.

### 2. Randomization sites

New header `kernel/mm/aslr.h`:

```c
#define MMAP_RND_BITS   28
#define STACK_RND_BITS  22
#define BRK_RND_BITS    13
extern int aslr_enabled;                 // cleared by `norandmaps`
uint64_t aslr_offset(unsigned bits);     // 0 if !aslr_enabled, else
                                         // (random_u64() & ((1<<bits)-1)) << PAGE_SHIFT
```

- `proc_alloc` / the spawn path: set `p->mmap_next = MMAP_BASE +
  aslr_offset(MMAP_RND_BITS)` instead of the bare constant. (`vma.c`
  already reads `p->mmap_next`; only the initial value changes.)
- `build_initial_stack` callers compute `stack_top = USER_STACK_TOP -
  aslr_offset(STACK_RND_BITS)` (16-byte aligned) and pass it down. The
  guard-page / slot math in `thread_stack_top_for` keys off
  `USER_STACK_TOP`; make it key off a per-process `p->stack_top_base`
  set once at spawn so secondary-thread stacks stay consistent with
  the randomized main stack.
- `elf_load`: for `e_type == ET_DYN`, choose `load_bias =
  ELF_ET_DYN_BASE + aslr_offset(MMAP_RND_BITS)`; add it to every
  `p_vaddr` and to the entry point; apply `R_X86_64_RELATIVE`
  relocations from `.rela.dyn`. For `ET_EXEC`, `load_bias = 0`
  (unchanged).
- `brk` init: `p->brk = page_up(bss_end) + aslr_offset(BRK_RND_BITS)`.
- `AT_RANDOM`: `random_bytes(buf, 16)` in `build_initial_stack`,
  replacing the LCG.

### 3. The disable knob

Parse `norandmaps` in the existing command-line handling (Multiboot2
cmdline). Set `aslr_enabled = 0`. `aslr_offset` returns 0 for every
caller, restoring today's deterministic layout exactly — so a test
build can pin addresses.

## Testing

- `kernel/dev/random_selftest.c`: draw 4096 bytes, assert not all-zero,
  assert a rough bit-balance (count of set bits within 40-60%), assert
  two consecutive `random_u64()` differ. `[random] selftest passed`.
- `userland/aslrtest.c`: `fork` several children; each prints the
  address of a stack variable, a `malloc`'d pointer, `&main`, and the
  `AT_RANDOM` bytes (via `getauxval`). Parent collects and asserts the
  stack and mmap addresses **differ across children** (with ASLR on)
  and are **identical** (with `norandmaps`). Two `make test` configs,
  or one run that re-execs itself — simplest: a second Makefile target
  `test-norandmaps`.
- The whole existing suite must still pass with ASLR **on** — this is
  the real test that nothing hard-codes an address. Anything that
  breaks reveals a latent assumption. Run the gauntlet with ASLR on.
- `mmaptest`, `tlstest`, `sigtest` (sigaltstack), `threadtest`
  (multiple thread stacks) are the ones most likely to surface a
  fixed-address assumption.

## ABI / stdlib impact

- `docs/stdlib.md`: `AT_RANDOM` is now backed by the entropy pool (was
  an LCG); layout is randomized by default; `norandmaps` disables it.
- `docs/abi-compatibility.md`: replace the "AT_RANDOM is not random"
  divergence with "AT_RANDOM is a non-certified software pool";
  document ASLR entropy bits vs Linux; note PIE `ET_DYN` load is now
  supported.
- New (future) `/dev/urandom`, `getrandom()` — out of scope here but
  the pool is built to back them.

## Risks

1. **A latent fixed-address assumption in the suite.** High
   probability something breaks the first time ASLR is on. That is the
   feature working. Budget time to chase each one; do not disable ASLR
   to make a test pass — fix the test or the code.
2. **Entropy quality.** RDTSC-jitter pools are weak on a quiet
   single-purpose VM. Acceptable for ASLR (defeats replay, not a
   determined analyst). Documented as non-certified. The RDRAND fold
   (post CPU-model bump) is the upgrade path.
3. **PIE relocation bugs** silently corrupt a `ET_DYN` binary. Mitigate
   by testing PIE support with a deliberately-built PIE test binary
   before wiring `ELF_ET_DYN_BASE` randomization — separate the two.
4. **Stack randomization vs the thread-stack slot allocator.** The
   guard-page arithmetic assumes a fixed `USER_STACK_TOP`. The
   `p->stack_top_base` change must be threaded through
   `thread_stack_top_for`, `thread_stack_alloc/free` and fork's
   `stack_slot` copy carefully — an off-by-one here is a stack overlap.

## Plan sketch (for `writing-plans`)

1. Entropy pool + selftest + timer/IRQ hooks. Gauntlet.
2. `aslr.h`, `aslr_offset`, `norandmaps` parse, `aslr_enabled`.
   No call sites yet. Gauntlet (no-op).
3. Randomize `mmap` base + `AT_RANDOM` from the pool. `aslrtest`
   (mmap/AT_RANDOM half). Gauntlet with ASLR on.
4. Randomize the main stack: `p->stack_top_base`, thread math,
   fork copy. `aslrtest` stack half + `threadtest`/`sigtest` still
   green. Gauntlet ×3.
5. PIE: `ET_DYN` load bias + `R_X86_64_RELATIVE`, tested with a
   purpose-built PIE binary (no randomization yet). Gauntlet.
6. Randomize the PIE load base + `brk`. Gauntlet ×3.
7. `test-norandmaps` Makefile target; docs.
