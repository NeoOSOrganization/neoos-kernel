# Debugging tools

Three instruments built in CS1, all **debug-build only** — none of them
exists in a shipping kernel, and with their flags off the generated
code is unchanged.

They exist because NeoOS is SMP with work stealing and has no host test
runner, so a rare interleaving is otherwise found only by luck. Every
SMP bug found before these tools was found by accident.

---

## 1. The poisoned / redzoned heap — `make DEBUG_HEAP=1`

Turns three classes of silent memory corruption into an immediate panic
naming the culprit. Boot confirms it is active:

```
[heap] initialized (debug: poison+redzone)
```

### What it catches

**Use after free.** Every freed size-class slot is filled with `0xDF`
past its 16-byte free-list metadata, and every slot handed out is
verified to still be poison:

```
[heap] PANIC: use-after-free write at slot=0xffff80000070cf80 offset=0x0000000000000020 value=0x0000000000000099
```

`offset` is the byte within the slot, `value` is what was written. The
panic fires at the *allocation* that discovered it, not at the write,
so the offending writer is whoever held that pointer after freeing it.

**Double free.** A free slot carries `HEAP_FREE_MAGIC`
(`0x5A5A5A5A5A5A5A5A`) at bytes `[8,16)`; allocation clears it. On
free, a slot still carrying the magic is only a *hint* — caller data
can happen to equal it — so the check confirms by walking the page's
free list before panicking:

```
[heap] PANIC: double free of slot=0xffff80000070dec0 class=0x0000000000000080
```

**Writing past the end of an allocation.** `kmalloc` fills
`[requested, size_class)` with `0xBB`; `kfree` checks it before
poisoning overwrites it:

```
[heap] PANIC: heap overrun past slot=0xffff80000070cf80 requested=0x0000000000000028 offset=0x0000000000000028 value=0x0000000000000077
```

`requested` is what the caller asked for; `offset` is the first byte
past it that was clobbered.

### How it works, and why that way

`kfree(ptr)` gets no size — it recovers the page with `ptr & ~0xFFF`
and reads `page->size_class`. The *slot* size is therefore always
known, but the **requested** size is not recorded anywhere, and the
redzone needs it. It goes in a `req[]` table in the page header.

Not in a per-allocation header in front of the slot, which would be the
obvious design: `struct heap_page` is `__attribute__((aligned(64)))`
precisely so every slot is aligned for the `fxsave`/`XSAVE` buffers
stored in heap-allocated objects, and `heap.c` carries a `#GP` in
`schedule()` on record from getting that wrong. A per-slot header would
shift every slot off that alignment.

### Known limitation

**Large allocations are not redzoned.** A large block's memory goes
straight back to the pmm on `kfree`, so there is nothing left to check
afterwards. Poison and redzone cover size-class allocations only. This
is deliberate, not an oversight.

---

## 2. Lock hold-time histograms — `make DEBUG_LOCKSTAT=1`

Answers "which lock is actually held a long time" with a number instead
of an architectural guess. Dumped at shutdown:

```
[lockstat] rank count max_tsc long_holds
[lockstat] 0x0000000000000008 0x00000000000008d4 0x00000000070ad2c7 0x00000000000008a1
```

| Column | Meaning |
|---|---|
| `rank` | the `LOCK_RANK_*` value (see `kernel/sync/lock.h`) |
| `count` | total acquisitions across all CPUs |
| `max_tsc` | longest single hold, in TSC ticks |
| `long_holds` | how many holds landed in the top bucket (>= 2^15 ticks) |

`long_holds` next to `count` is the interesting ratio. The line above
is real: rank 8 (`LOCK_RANK_TTY` / `LOCK_RANK_DRIVER`) put 2209 of 2260
holds in the top bucket, with a maximum around 118M ticks — the console
render path holding a tty lock across a full framebuffer paint. That is
a CS5 target, and the baseline to measure any fix against.

### Constraints it respects

- **It never takes a lock.** Statistics accumulate in per-CPU rows
  found in O(1) via an index cached in the CPU block. A lock inside the
  lock path deadlocks the first time two CPUs contend.
- **It stays small.** A naive `[256 ranks][32 buckets]` table per CPU is
  64 KiB, and `MAX_CPUS` is 128 — nearly 9 MiB of `.bss` for a debug
  counter. Ranks are sparse (0–21, then the leaf block 250–255), so
  they fold into 38 slots with 16 buckets: ~5.5 KiB per CPU.
- **It does not disturb `struct cpu`'s layout.** That struct's `CPU_*`
  byte offsets are mirrored in assembly by eye, so only the small
  acquire-timestamp array and the index are appended, at the very end.
  The histograms live in `lock.c`.

Ranks between 32 and 249 are not counted — none exist today. Adding one
means extending `lockstat_slot()`.

---

## 3. The gauntlet — `tools/gauntlet.sh [N] [CONC]`

```bash
tools/gauntlet.sh 15 3
```

Builds once, boots N kernels concurrently under headless QEMU, and
applies the same checks `make test` does — `FAILED` lines, the boot
marker, and every `REQUIRED_MARKERS` entry, parsed live out of the
Makefile so it cannot drift.

**A single green `make test` is not sign-off for anything
timing-dependent.** `PGAUNTLET PASSED: 15/15` is the bar for any change
touching locks, paging, or scheduling.

### Reading the output

Failures are classified. A `HARD` signature — any `PANIC` (including
the debug heap's), a fault, a lifetime/steal/parallelism selftest
failure — fails immediately and is never retried. A `FLAKY` one (host
contention starving the emulated ATA controller or the guest clock, a
missing marker with no hard signature) is retried once, solo.

Since CS1 it also prints a per-marker flakiness table:

```
--- per-marker flakiness over 15 run(s) ---
    6%   1/15  [evtest] ALL PASSED
```

A marker that misses one run in fifteen is now a tracked number rather
than something hidden behind a green checkmark.

### Running the instrumented kernel

`GAUNTLET_MAKEFLAGS` passes make variables through to the build:

```bash
GAUNTLET_MAKEFLAGS=DEBUG_HEAP=1 tools/gauntlet.sh 15 3
```

It is empty by default on purpose — the gauntlet's ordinary job is to
exercise what ships, and it deliberately builds without
`-DNEOOS_TEST_HOOKS` for the same reason.

---

## Running faster: `KVM=1`

```bash
make KVM=1 test
KVM=1 tools/gauntlet.sh 15 3
```

Swaps TCG emulation for hardware virtualisation (`-enable-kvm -cpu
host`). Measured on this machine:

| | single boot | gauntlet 15x3 |
|---|---|---|
| TCG, `-cpu Nehalem` (default) | 9.11 s | 110.6 s |
| KVM, `-cpu host` | 3.90 s | 37.0 s |

Roughly **3x on the gauntlet**, which is where the time goes.

**The default stays TCG/Nehalem deliberately**, for two reasons. The
gauntlet's `FLAKY_RE` signatures are tuned for TCG's host-contention
artifacts (a starved emulated ATA controller, a guest clock that does
not advance), and `-cpu host` exposes whatever the host CPU has, so the
feature set the tests cover stops being pinned. Sign off on the default;
iterate with `KVM=1`.

One property makes `KVM=1` more than a speed knob: **TCG round-robins
the vCPUs, KVM runs them genuinely in parallel.** SMP interleavings that
need true simultaneity are reachable under KVM and not under TCG, so a
race the CS track is hunting may only ever appear there. A failure under
`KVM=1` that does not reproduce under TCG is therefore worth taking
seriously rather than dismissing as an accelerator artifact.

Requires access to `/dev/kvm` (group membership or a POSIX ACL).

## Combining them

The heap poisoner and the lock statistics are independent flags and can
be combined with each other and with `DEBUG_STOP_WINDOW`:

```bash
make DEBUG_HEAP=1 DEBUG_LOCKSTAT=1 test
```

For a stress run where corruption must be *localizable* rather than
merely detectable — which is the whole reason CS1 precedes CS2's
regression tests — use `DEBUG_HEAP=1` under the gauntlet.
