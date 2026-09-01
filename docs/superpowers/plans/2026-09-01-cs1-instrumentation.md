# CS1 — Instrumentation — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the three tools every later CS sub-milestone depends on:
a poisoned/redzoned heap that turns silent corruption into a named
panic, per-rank lock hold-time histograms, and a promoted gauntlet that
reports per-marker flakiness instead of a single pass/fail.

**Architecture:** All three are debug-build-only and cost nothing in a
shipping build. The heap gains poison-on-free, a use-after-free check
on reuse, a double-free check, and a redzone whose size comes from a
per-page table of requested sizes (the heap records no per-allocation
size today, and a per-slot header would break the 16/64-byte alignment
`fxsave`/`XSAVE` depend on). Lock statistics accumulate in per-CPU
arrays — never a lock inside the lock path — and are summed only at
dump time. The gauntlet moves to `tools/gauntlet.sh` and grows a
per-marker miss table.

**Tech Stack:** C (freestanding), x86-64, GNU Make, bash, headless
QEMU, the parallel gauntlet (CONC=3).

**Spec:** `docs/superpowers/specs/2026-09-01-concurrency-and-scaling-design.md` (CS1)

## Global Constraints

- **No host unit tests.** Every test is a kernel selftest writing
  through `serial_write_string`, or a userland binary ending in a
  `[name] ALL PASSED` marker wired into the Makefile's
  `REQUIRED_MARKERS`.
- **The gauntlet is the bar.** After each task,
  `tools/gauntlet.sh 15 3` (before Task 6:
  `.superpowers/sdd/2026-08-31-phase14-input-and-solidity/pgauntlet.sh 15 3`)
  must report `PGAUNTLET PASSED: 15/15`.
- **Debug builds must not change shipping behaviour.** Every feature
  here sits behind a `#ifdef`; with the flag off, the generated code
  must be identical to today's. Task 1 establishes that and each later
  task re-checks it.
- **Never take a lock inside the lock path.** Task 5's statistics use
  per-CPU storage and plain arithmetic only. Anything else deadlocks
  the first time two CPUs contend.
- **`struct cpu` has asserted byte offsets.** `kernel/arch/cpu_local.h`
  carries `_Static_assert`s on `CPU_*` offsets used from assembly, and
  a comment warning that field order matters. **Append new fields at
  the end of the struct**, never in the middle.
- **CS1 changes nothing a user program can observe**, so there is no
  `docs/stdlib.md` or `docs/abi-compatibility.md` work.

## Design decisions, and why

Two constraints from the current heap shape everything in Tasks 2-4,
and are worth stating before the tasks assume them:

1. **`kfree` gets no size.** `kfree(ptr)` recovers the page with
   `ptr & ~0xFFF` and reads `page->size_class`. So the *slot* size is
   always known at free time, but the **requested** size is not
   recorded anywhere. Poisoning (Task 2) and double-free detection
   (Task 3) need only the slot size and land first; the redzone
   (Task 4) needs the requested size and pays for a table to get it.
2. **Slot alignment is load-bearing.** `struct heap_page` is
   `__attribute__((aligned(64)))` specifically so every slot address is
   suitably aligned for `fxsave`/`XSAVE` buffers stored in
   heap-allocated objects; `heap.c`'s own comment records a `#GP` in
   `schedule()` from getting this wrong. **A per-allocation header in
   front of the slot is therefore not available to us** — it would
   shift every slot by its own size. Task 4 puts the requested-size
   table in the *page header* instead, which keeps the slot area
   starting at a 64-byte multiple.

Byte values, used consistently:

| Byte | Meaning |
|---|---|
| `0xDF` | freed slot body ("dead free") |
| `0xBB` | redzone between the requested size and the slot end |
| `0x5A5A5A5A5A5A5A5A` | `HEAP_FREE_MAGIC`, at slot bytes `[8,16)` while free |

---

## Task 1: The `NEOOS_DEBUG_HEAP` build switch

Establishes the flag and proves both builds boot, before any behaviour
hangs off it.

**Files:**
- Modify: `Makefile:8-14` (flag plumbing)
- Modify: `kernel/mm/heap.c` (report the mode)

**Interfaces:**
- Produces: `NEOOS_DEBUG_HEAP`, defined when `make DEBUG_HEAP=1`. Tasks
  2-4 hang everything off it.

- [ ] **Step 1: Add the flag next to the existing one**

`Makefile` already threads `NEOOS_DEBUG_STOP_WINDOW` this way at lines
8-10. Add immediately after it:

```make
ifdef DEBUG_HEAP
CFLAGS += -DNEOOS_DEBUG_HEAP
endif
```

- [ ] **Step 2: Make the mode visible at boot**

In `kernel/mm/heap.c`, in `heap_init`, replace the existing line:

```c
    serial_write_string("[heap] initialized\n");
```

with:

```c
#ifdef NEOOS_DEBUG_HEAP
    serial_write_string("[heap] initialized (debug: poison+redzone)\n");
#else
    serial_write_string("[heap] initialized\n");
#endif
```

- [ ] **Step 3: Verify both builds**

```bash
make clean-kernel && make build && echo "PRODUCTION BUILD OK"
make clean-kernel && make DEBUG_HEAP=1 build && echo "DEBUG BUILD OK"
```

Expected: both print their OK line with no errors.

- [ ] **Step 4: Verify the debug build boots and says so**

```bash
make clean-kernel && make DEBUG_HEAP=1 test 2>&1 | tail -2
grep -E "^\[heap\]" build/serial.log
```

Expected: `PASS: ...` and `[heap] initialized (debug: poison+redzone)`.

- [ ] **Step 5: Verify the production build is unchanged**

```bash
make clean-kernel && make test 2>&1 | tail -2
grep -E "^\[heap\]" build/serial.log
```

Expected: `PASS: ...` and `[heap] initialized` with no debug suffix.

- [ ] **Step 6: Gauntlet and commit**

```bash
.superpowers/sdd/2026-08-31-phase14-input-and-solidity/pgauntlet.sh 15 3
git add Makefile kernel/mm/heap.c
git commit -m "CS1: a DEBUG_HEAP build switch, reported at boot"
```

---

## Task 2: Poison on free, verify on reuse

Catches use-after-free writes and reads of stale freed memory.

**Files:**
- Modify: `kernel/mm/heap.c` (`kmalloc_locked`, `kfree_locked`, new helpers)
- Test: `kernel/mm/heap.c` (`heap_selftest`)

**Interfaces:**
- Consumes: `NEOOS_DEBUG_HEAP` (Task 1).
- Produces: `HEAP_POISON`, `HEAP_FREE_MAGIC`, `HEAP_META_BYTES`,
  `heap_poison_slot()`, `heap_check_poison()` — all file-static in
  `heap.c`, used by Tasks 3 and 4. Nothing is added to `heap.h`; the
  check runs from inside `heap_selftest`, which is already declared
  there.

- [ ] **Step 1: Write the failing test**

Add to `kernel/mm/heap.c`, above `heap_selftest`:

```c
#ifdef NEOOS_DEBUG_HEAP
// A freed slot must read back as poison, and reusing it must hand back
// a slot whose body is still poison (nothing else scribbled on it).
static int heap_poison_check(void) {
    uint8_t *p = kmalloc(64);
    if (!p) { return 1; }
    for (int i = 0; i < 64; i++) { p[i] = 0x11; }
    kfree(p);
    // The body past the free-list metadata must be poison now.
    for (int i = 16; i < 64; i++) {
        if (p[i] != HEAP_POISON) { return 1; }
    }
    // And the next same-class allocation gets a poisoned slot back.
    uint8_t *q = kmalloc(64);
    if (!q) { return 1; }
    kfree(q);
    return 0;
}
#endif
```

Call it from `heap_selftest`, immediately before its final
`serial_write_string("[heap] selftest passed\n");`:

```c
#ifdef NEOOS_DEBUG_HEAP
    if (heap_poison_check()) {
        serial_write_string("[heap] selftest FAILED: poison not applied on free\n");
        return;
    }
#endif
```

- [ ] **Step 2: Run it to see it fail**

```bash
make clean-kernel && make DEBUG_HEAP=1 test 2>&1 | tail -3
```

Expected: build fails — `HEAP_POISON` is undefined. That compile error
is the failing state.

- [ ] **Step 3: Implement poisoning**

In `kernel/mm/heap.c`, after the `HEAP_LARGE_MARKER` define, add:

```c
#ifdef NEOOS_DEBUG_HEAP
#define HEAP_POISON      0xDFu
#define HEAP_FREE_MAGIC  0x5A5A5A5A5A5A5A5AULL

// A free slot's first 16 bytes are metadata: [0,8) is the free-list
// link, [8,16) is the magic. Only [16, size_class) is poisoned, so the
// smallest class (16) has no poisoned body and is covered by the magic
// and free-list checks alone.
#define HEAP_META_BYTES 16

static void heap_poison_slot(void *slot, uint32_t size_class) {
    uint8_t *b = (uint8_t *)slot;
    for (uint32_t i = HEAP_META_BYTES; i < size_class; i++) { b[i] = HEAP_POISON; }
}

// Panics naming the slot and the first byte that is not poison.
static void heap_check_poison(void *slot, uint32_t size_class) {
    uint8_t *b = (uint8_t *)slot;
    for (uint32_t i = HEAP_META_BYTES; i < size_class; i++) {
        if (b[i] != HEAP_POISON) {
            serial_write_string("[heap] PANIC: use-after-free write at slot=");
            serial_write_hex64((uint64_t)(uintptr_t)slot);
            serial_write_string(" offset=");
            serial_write_hex64((uint64_t)i);
            serial_write_string(" value=");
            serial_write_hex64((uint64_t)b[i]);
            serial_write_string("\n");
            for (;;) { __asm__ volatile ("cli; hlt"); }
        }
    }
}
#endif
```

`struct heap_free_slot` gains the magic field so `[8,16)` is named
rather than implicit:

```c
struct heap_free_slot {
    struct heap_free_slot *next;
#ifdef NEOOS_DEBUG_HEAP
    uint64_t magic;
#endif
};
```

In `kfree_locked`, in the size-class path, poison before linking. The
existing tail of that function becomes:

```c
    struct heap_free_slot *slot = (struct heap_free_slot *)ptr;
#ifdef NEOOS_DEBUG_HEAP
    heap_poison_slot(ptr, page->size_class);
    slot->magic = HEAP_FREE_MAGIC;
#endif
    slot->next = page->free_list;
    page->free_list = slot;
    page->meta++;
```

In `kmalloc_locked`, verify on the way out. The existing lines:

```c
        struct heap_free_slot *slot = page->free_list;
        page->free_list = slot->next;
        page->meta--;
        return (void *)slot;
```

become:

```c
        struct heap_free_slot *slot = page->free_list;
        page->free_list = slot->next;
        page->meta--;
#ifdef NEOOS_DEBUG_HEAP
        heap_check_poison(slot, page->size_class);
        slot->magic = 0;
#endif
        return (void *)slot;
```

Note the fresh slots built by `heap_new_page` are never poisoned, so
`heap_check_poison` must not run on them. It does not: they are handed
out through the same path, so poison them at carve time — in
`heap_new_page`'s slot loop, after `page->free_list = slot;` add:

```c
#ifdef NEOOS_DEBUG_HEAP
        heap_poison_slot(slot, size_class);
        slot->magic = HEAP_FREE_MAGIC;
#endif
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
make clean-kernel && make DEBUG_HEAP=1 test 2>&1 | tail -2
grep -E "^\[heap\]" build/serial.log
```

Expected: `PASS`, and `[heap] selftest passed`.

- [ ] **Step 5: Prove the detector actually detects**

Temporarily add a real use-after-free to `heap_poison_check`, right
after the `kfree(p)`:

```c
    p[32] = 0x99;   // TEMPORARY -- CS1 Task 2 detector proof
```

```bash
make clean-kernel && make DEBUG_HEAP=1 test 2>&1 | tail -3
grep -E "use-after-free" build/serial.log
```

Expected: `[heap] PANIC: use-after-free write at slot=... offset=0x20
value=0x99`. **Then remove the temporary line and rebuild.** A detector
that has never fired is not evidence of anything.

- [ ] **Step 6: Verify the production build is untouched**

```bash
make clean-kernel && make test 2>&1 | tail -2
```

Expected: `PASS`.

- [ ] **Step 7: Gauntlet and commit**

```bash
.superpowers/sdd/2026-08-31-phase14-input-and-solidity/pgauntlet.sh 15 3
git add kernel/mm/heap.c
git commit -m "CS1: poison freed heap slots, check them on reuse"
```

---

## Task 3: Double-free detection

**Files:**
- Modify: `kernel/mm/heap.c` (`kfree_locked`)
- Test: `kernel/mm/heap.c`

**Interfaces:**
- Consumes: `HEAP_FREE_MAGIC`, `HEAP_META_BYTES` (Task 2).

- [ ] **Step 1: Write the failing test**

The check panics, so the selftest cannot call it directly — it proves
the *negative* (a legitimate alloc/free/alloc/free cycle never trips
it), and Step 4 proves the positive by hand. Extend
`heap_poison_check` in `kernel/mm/heap.c`, before its `return 0;`:

```c
    // A slot freed, reallocated and freed again must not look like a
    // double free: reallocation clears the magic.
    uint8_t *r = kmalloc(128);
    if (!r) { return 1; }
    kfree(r);
    uint8_t *r2 = kmalloc(128);
    if (!r2) { return 1; }
    kfree(r2);
```

- [ ] **Step 2: Implement the check**

In `kernel/mm/heap.c`, add above `kfree_locked`:

```c
#ifdef NEOOS_DEBUG_HEAP
// The magic alone is a hint, not proof: caller data can happen to equal
// it. Confirm by walking the page's free list, which is O(free slots)
// and only ever runs on that hint.
static int heap_on_free_list(struct heap_page *page, struct heap_free_slot *slot) {
    for (struct heap_free_slot *s = page->free_list; s; s = s->next) {
        if (s == slot) { return 1; }
    }
    return 0;
}
#endif
```

and, in `kfree_locked`'s size-class path, before the poisoning added in
Task 2:

```c
    struct heap_free_slot *slot = (struct heap_free_slot *)ptr;
#ifdef NEOOS_DEBUG_HEAP
    if (slot->magic == HEAP_FREE_MAGIC && heap_on_free_list(page, slot)) {
        serial_write_string("[heap] PANIC: double free of slot=");
        serial_write_hex64((uint64_t)(uintptr_t)slot);
        serial_write_string(" class=");
        serial_write_hex64((uint64_t)page->size_class);
        serial_write_string("\n");
        for (;;) { __asm__ volatile ("cli; hlt"); }
    }
    heap_poison_slot(ptr, page->size_class);
    slot->magic = HEAP_FREE_MAGIC;
#endif
```

- [ ] **Step 3: Run the tests**

```bash
make clean-kernel && make DEBUG_HEAP=1 test 2>&1 | tail -2
grep -E "^\[heap\]" build/serial.log
```

Expected: `PASS`, `[heap] selftest passed` — the legitimate cycle does
not trip the check.

- [ ] **Step 4: Prove the detector detects**

Temporarily add to `heap_poison_check`, after its final `kfree(r2)`:

```c
    kfree(r2);   // TEMPORARY -- CS1 Task 3 detector proof (double free)
```

```bash
make clean-kernel && make DEBUG_HEAP=1 test 2>&1 | tail -3
grep -E "double free" build/serial.log
```

Expected: `[heap] PANIC: double free of slot=... class=0x80`. **Remove
the temporary line and rebuild.**

- [ ] **Step 5: Gauntlet and commit**

```bash
make clean-kernel && make test 2>&1 | tail -2   # production still green
.superpowers/sdd/2026-08-31-phase14-input-and-solidity/pgauntlet.sh 15 3
git add kernel/mm/heap.c
git commit -m "CS1: detect double frees via a free magic plus a free-list walk"
```

---

## Task 4: Redzone on the requested size

The piece that catches "wrote past the end of a `kmalloc`'d node" —
CS2.1's radix-tree bug is exactly this shape.

**Files:**
- Modify: `kernel/mm/heap.c` (`struct heap_page`, `heap_new_page`,
  `kmalloc_locked`, `kfree_locked`)
- Test: `kernel/mm/heap.c`

**Interfaces:**
- Consumes: Tasks 2 and 3.
- Produces: nothing external.

- [ ] **Step 1: Write the failing test**

Extend `heap_poison_check` in `kernel/mm/heap.c`, before `return 0;`:

```c
    // A 40-byte request lands in the 64 class; bytes [40,64) are
    // redzone and must survive an allocation that writes exactly 40.
    uint8_t *z = kmalloc(40);
    if (!z) { return 1; }
    for (int i = 0; i < 40; i++) { z[i] = 0x77; }
    kfree(z);   // panics if the redzone was clobbered
```

- [ ] **Step 2: Implement the requested-size table**

The table lives in the page header so slot alignment is untouched.
Replace `struct heap_page` with:

```c
#ifdef NEOOS_DEBUG_HEAP
// Largest slot count any class can produce: the smallest class is 16
// bytes over a 4096-byte frame.
#define HEAP_MAX_SLOTS (PMM_FRAME_SIZE / 16)
#endif

struct heap_page {
    struct heap_page *next;
    struct heap_free_slot *free_list;
    uint32_t size_class; // bytes per slot, or HEAP_LARGE_MARKER for a large (multi-page) allocation
    uint32_t meta;        // size-class pages: free slot count. Large allocations: the pmm buddy order.
#ifdef NEOOS_DEBUG_HEAP
    // Requested size per slot index, so kfree can find where the
    // redzone starts. In the header rather than in front of each slot:
    // a per-slot header would shift every slot off the 64-byte
    // alignment that fxsave/XSAVE buffers in heap objects depend on.
    uint16_t req[HEAP_MAX_SLOTS];
#endif
} __attribute__((aligned(64)));
```

Add the helpers, after `heap_check_poison`:

```c
#ifdef NEOOS_DEBUG_HEAP
#define HEAP_REDZONE 0xBBu

static uint32_t heap_slot_index(struct heap_page *page, void *slot) {
    uint8_t *area = (uint8_t *)page + sizeof(struct heap_page);
    return (uint32_t)(((uint8_t *)slot - area) / page->size_class);
}

static void heap_set_redzone(struct heap_page *page, void *slot, uint32_t req) {
    uint8_t *b = (uint8_t *)slot;
    page->req[heap_slot_index(page, slot)] = (uint16_t)req;
    for (uint32_t i = req; i < page->size_class; i++) { b[i] = HEAP_REDZONE; }
}

static void heap_check_redzone(struct heap_page *page, void *slot) {
    uint8_t *b = (uint8_t *)slot;
    uint32_t req = page->req[heap_slot_index(page, slot)];
    if (req == 0 || req > page->size_class) { return; }   // never allocated with a size
    for (uint32_t i = req; i < page->size_class; i++) {
        if (b[i] != HEAP_REDZONE) {
            serial_write_string("[heap] PANIC: heap overrun past slot=");
            serial_write_hex64((uint64_t)(uintptr_t)slot);
            serial_write_string(" requested=");
            serial_write_hex64((uint64_t)req);
            serial_write_string(" offset=");
            serial_write_hex64((uint64_t)i);
            serial_write_string(" value=");
            serial_write_hex64((uint64_t)b[i]);
            serial_write_string("\n");
            for (;;) { __asm__ volatile ("cli; hlt"); }
        }
    }
}
#endif
```

`kmalloc_locked` must remember the size the *caller* asked for, before
the minimum-size bump. At the very top of `kmalloc_locked`, capture it:

```c
static void *kmalloc_locked(size_t size) {
    if (size == 0) {
        return 0;
    }
#ifdef NEOOS_DEBUG_HEAP
    size_t requested = size;
#endif
    if (size < sizeof(struct heap_free_slot)) {
        size = sizeof(struct heap_free_slot);
    }
```

and in the size-class return path, after Task 2's poison check:

```c
#ifdef NEOOS_DEBUG_HEAP
        heap_check_poison(slot, page->size_class);
        slot->magic = 0;
        heap_set_redzone(page, slot, (uint32_t)requested);
#endif
        return (void *)slot;
```

In `kfree_locked`, check the redzone **before** poisoning (poisoning
overwrites it). Insert immediately after the double-free check from
Task 3:

```c
    heap_check_redzone(page, ptr);
    page->req[heap_slot_index(page, ptr)] = 0;
```

- [ ] **Step 3: Run the tests**

```bash
make clean-kernel && make DEBUG_HEAP=1 test 2>&1 | tail -2
grep -E "^\[heap\]" build/serial.log
```

Expected: `PASS`, `[heap] selftest passed`.

- [ ] **Step 4: Prove the detector detects**

Temporarily change the write in the Step 1 test from 40 bytes to 48,
overrunning the request by 8:

```c
    for (int i = 0; i < 48; i++) { z[i] = 0x77; }   // TEMPORARY -- overrun
```

```bash
make clean-kernel && make DEBUG_HEAP=1 test 2>&1 | tail -3
grep -E "heap overrun" build/serial.log
```

Expected: `[heap] PANIC: heap overrun past slot=... requested=0x28
offset=0x28 value=0x77`. **Restore 40 and rebuild.**

- [ ] **Step 5: Confirm the debug heap still has room to boot**

The header grew by `HEAP_MAX_SLOTS * 2` = 512 bytes, so every
size-class page carves fewer slots. Confirm the debug build still boots
all suites:

```bash
make clean-kernel && make DEBUG_HEAP=1 test 2>&1 | tail -2
```

Expected: `PASS`. If frames run short, report it rather than shrinking
`HEAP_MAX_SLOTS` — the smallest class genuinely can produce that many
slots, and a smaller table would index out of bounds.

- [ ] **Step 6: Gauntlet (both builds) and commit**

```bash
make clean-kernel && make test 2>&1 | tail -2
.superpowers/sdd/2026-08-31-phase14-input-and-solidity/pgauntlet.sh 15 3
git add kernel/mm/heap.c
git commit -m "CS1: redzone every kmalloc slot past its requested size"
```

---

## Task 5: Per-rank lock hold-time histograms

Turns "the global `vfs_lock` is a throughput killer" into a number CS5
can move.

**Files:**
- Modify: `kernel/arch/cpu_local.h` (append fields to `struct cpu`)
- Modify: `kernel/sync/lock.c` (`spin_lock_irqsave`, `spin_unlock_irqrestore`)
- Modify: `kernel/sync/lock.h` (declare the dump)
- Modify: `Makefile` (`DEBUG_LOCKSTAT` flag)
- Modify: `kernel/kernel.c` (dump at shutdown)

**Interfaces:**
- Produces: `void lock_stats_dump(void);` — prints `[lockstat]` lines.
  No-op when `NEOOS_DEBUG_LOCKSTAT` is off.

- [ ] **Step 1: Add the flag**

In `Makefile`, after Task 1's `DEBUG_HEAP` block:

```make
ifdef DEBUG_LOCKSTAT
CFLAGS += -DNEOOS_DEBUG_LOCKSTAT
endif
```

- [ ] **Step 2: Add per-CPU accumulators**

**Sizing matters here.** A naive `[256 ranks][32 buckets]` table per
CPU is `256*32*8` bytes = 64 KiB per CPU, and `MAX_CPUS` is 128 — nearly
9 MiB of `.bss` for a debug counter. Two compressions avoid that:
ranks are sparse (0-21, then 250-255), so they fold into 38 slots; and
16 buckets cover holds up to 2^16 ticks with everything above in the
top bucket. That is ~5.5 KiB per CPU, ~700 KiB total, and only in a
`DEBUG_LOCKSTAT` build.

The histograms live in `lock.c` rather than in `struct cpu`, because
that struct has `CPU_*` byte offsets kept in sync by eye with assembly
and is on the hot per-CPU cacheline. Only the small acquire-timestamp
array goes in the struct, **appended at the very end**:

```c
#ifdef NEOOS_DEBUG_LOCKSTAT
    // CS1: TSC at acquire, per held-stack depth. The histograms live in
    // lock.c -- see the sizing note there. Appended at the END of the
    // struct: the CPU_* offsets above are mirrored in assembly by eye.
    uint64_t lockstat_acquire_tsc[LOCK_MAX_HELD];
    int      lockstat_index;        // this block's index into lockstats[]
#endif
```

Set `lockstat_index` where each block is initialised in
`kernel/arch/cpu_local.c` — both `cpu_local_init_bsp` (index 0) and
`cpu_local_init_ap(int index)` already know their index:

```c
#ifdef NEOOS_DEBUG_LOCKSTAT
    c->lockstat_index = index;      // 0 in the BSP path
#endif
```

`lock.c` needs to index its tables by CPU. There is no CPU-index field
and no exported `cpus[]` accessor, so add one. In
`kernel/arch/cpu_local.h`, next to `cpu_local_init_ap`:

```c
// The per-CPU block for an arbitrary index, for code that aggregates
// across CPUs (lock statistics). this_cpu() remains the only fast path.
struct cpu *cpu_at(int index);
```

and in `kernel/arch/cpu_local.c`, where `struct cpu cpus[MAX_CPUS];`
is defined:

```c
struct cpu *cpu_at(int index) {
    if (index < 0 || index >= MAX_CPUS) { return 0; }
    return &cpus[index];
}
```

- [ ] **Step 3: Timestamp acquire and bucket release**

In `kernel/sync/lock.c`, add near the top:

```c
#ifdef NEOOS_DEBUG_LOCKSTAT
#include "arch/cpu_local.h"
#include "smp/smp.h"

// Ranks are sparse: 0..21 today, plus the leaf block 250..255. Fold
// them into a compact slot so the tables stay small (see the sizing
// note in the plan): 9 MiB naive, ~700 KiB this way.
#define LOCKSTAT_LOW    32
#define LOCKSTAT_SLOTS  (LOCKSTAT_LOW + 6)
#define LOCKSTAT_BUCKETS 16

static inline int lockstat_slot(uint8_t rank) {
    if (rank < LOCKSTAT_LOW) { return rank; }
    if (rank >= 250) { return LOCKSTAT_LOW + (rank - 250); }
    return -1;                       // a rank added between 32 and 249
}

struct lockstat {
    uint64_t count[LOCKSTAT_SLOTS];
    uint64_t max[LOCKSTAT_SLOTS];
    uint64_t buckets[LOCKSTAT_SLOTS][LOCKSTAT_BUCKETS];
};
static struct lockstat lockstats[MAX_CPUS];

static inline uint64_t lockstat_now(void) {
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}
#endif
```

`lockstats` is indexed by CPU, and the lock path must reach its own row
without a lock. `this_cpu()` gives the block; its index is
`lapic_id`-independent, so derive it once with `cpu_at`:

```c
#ifdef NEOOS_DEBUG_LOCKSTAT
// O(1), using the index cached in the block by Step 2. A scan over
// cpu_at() would run on every single unlock and is not acceptable even
// in a debug build.
static inline struct lockstat *lockstat_mine(struct cpu *c) {
    int i = c->lockstat_index;
    return &lockstats[(i >= 0 && i < MAX_CPUS) ? i : 0];
}
#endif
```

`cpu_at()` is therefore needed only by aggregation code, not by the
lock path.

In `spin_lock_irqsave`, after the existing push:

```c
    c->held_names[c->held_depth] = l->name;
    c->held_ranks[c->held_depth++] = l->rank;
#ifdef NEOOS_DEBUG_LOCKSTAT
    c->lockstat_acquire_tsc[c->held_depth - 1] = lockstat_now();
#endif
    return flags;
```

In `spin_unlock_irqrestore`, after the `held_depth <= 0` guard and
before the depth is decremented — read the current body first with
`sed -n '132,160p' kernel/sync/lock.c` so this lands correctly relative
to the existing pop:

```c
#ifdef NEOOS_DEBUG_LOCKSTAT
    {
        int sl = lockstat_slot(l->rank);
        if (sl >= 0) {
            uint64_t held = lockstat_now() - c->lockstat_acquire_tsc[c->held_depth - 1];
            struct lockstat *ls = lockstat_mine(c);
            ls->count[sl]++;
            if (held > ls->max[sl]) { ls->max[sl] = held; }
            unsigned b = 0;
            while (b < LOCKSTAT_BUCKETS - 1 && (held >> (b + 1))) { b++; }
            ls->buckets[sl][b]++;
        }
    }
#endif
```

- [ ] **Step 4: Add the dump**

In `kernel/sync/lock.h`, next to `lock_held_depth`:

```c
// Prints per-rank hold-time statistics gathered by a DEBUG_LOCKSTAT
// build. A no-op otherwise.
void lock_stats_dump(void);
```

In `kernel/sync/lock.c`:

```c
void lock_stats_dump(void) {
#ifdef NEOOS_DEBUG_LOCKSTAT
    // rank, total acquisitions, longest hold in TSC ticks, and how many
    // holds landed in the top bucket (>= 2^15 ticks) -- the column that
    // says "this lock is held a long time", which is what CS5's
    // vfs_lock and poll_broadcast work needs to move.
    serial_write_string("[lockstat] rank count max_tsc long_holds\n");
    for (int sl = 0; sl < LOCKSTAT_SLOTS; sl++) {
        uint64_t count = 0, max = 0, longh = 0;
        for (int i = 0; i < MAX_CPUS; i++) {
            count += lockstats[i].count[sl];
            if (lockstats[i].max[sl] > max) { max = lockstats[i].max[sl]; }
            longh += lockstats[i].buckets[sl][LOCKSTAT_BUCKETS - 1];
        }
        if (!count) { continue; }
        uint64_t rank = (sl < LOCKSTAT_LOW) ? (uint64_t)sl
                                            : (uint64_t)(250 + (sl - LOCKSTAT_LOW));
        serial_write_string("[lockstat] ");
        serial_write_hex64(rank);
        serial_write_string(" ");
        serial_write_hex64(count);
        serial_write_string(" ");
        serial_write_hex64(max);
        serial_write_string(" ");
        serial_write_hex64(longh);
        serial_write_string("\n");
    }
#endif
}
```

- [ ] **Step 5: Call it at shutdown**

In `kernel/kernel.c`, at the shutdown path that prints
`"[kernel] shutdown requested, powering off"`, add `lock_stats_dump();`
immediately before that line.

- [ ] **Step 6: Run it**

```bash
make clean-kernel && make DEBUG_LOCKSTAT=1 test 2>&1 | tail -2
grep -c "^\[lockstat\]" build/serial.log
grep "^\[lockstat\]" build/serial.log | head -12
```

Expected: `PASS`, and a dozen-ish `[lockstat]` lines. Rank `0x8` (TTY /
DRIVER) and `0x11` (HEAP) should both appear with large counts — if
nothing appears, the dump ran before any lock was taken, or the flag is
not reaching the compile.

- [ ] **Step 7: Verify both other builds still pass, gauntlet, commit**

```bash
make clean-kernel && make test 2>&1 | tail -2
make clean-kernel && make DEBUG_HEAP=1 test 2>&1 | tail -2
.superpowers/sdd/2026-08-31-phase14-input-and-solidity/pgauntlet.sh 15 3
git add Makefile kernel/arch/cpu_local.h kernel/sync/lock.c kernel/sync/lock.h kernel/kernel.c
git commit -m "CS1: per-rank lock hold-time histograms behind DEBUG_LOCKSTAT"
```

---

## Task 6: Promote the gauntlet and report flakiness

The script exists but lives in one milestone's working directory, so no
later plan can point at it. It also answers only pass/fail, when what
CS2-CS5 need is *which marker is flaky and how often*.

**Files:**
- Move: `.superpowers/sdd/2026-08-31-phase14-input-and-solidity/pgauntlet.sh` → `tools/gauntlet.sh`
- Modify: the moved script (paths, flakiness table, debug-build passthrough)
- Modify: `docs/superpowers/specs/2026-09-01-concurrency-and-scaling-design.md`,
  `docs/superpowers/specs/2026-08-31-post-smp-roadmap.md` (references)

- [ ] **Step 1: Move it, preserving history**

```bash
mkdir -p tools
git mv .superpowers/sdd/2026-08-31-phase14-input-and-solidity/pgauntlet.sh tools/gauntlet.sh
```

- [ ] **Step 2: Repoint its internal paths**

The script sets `DIR` to its old milestone directory and writes its
work tree and failure logs there. Change:

```bash
DIR=.superpowers/sdd/2026-08-31-phase14-input-and-solidity
WORK=$DIR/pgauntlet
```

to:

```bash
DIR=build/gauntlet
WORK=$DIR/work
mkdir -p "$DIR"
```

and the two `rm -f "$DIR"/pgauntlet.serial.run*` /
`cp ... "$DIR/pgauntlet.serial.run$i"` sites keep working unchanged
against the new `DIR`.

- [ ] **Step 3: Let it build a debug kernel**

The script deliberately builds without `-DNEOOS_TEST_HOOKS` ("the
gauntlet's job is to exercise what ships"). CS2 and CS3 need it to be
able to run the *poisoned* kernel too. Change the build line:

```bash
if ! make clean-kernel iso disk-image > "$WORK/build.log" 2>&1; then
```

to:

```bash
# GAUNTLET_MAKEFLAGS lets a caller run the poisoned/instrumented kernel
# (e.g. GAUNTLET_MAKEFLAGS="DEBUG_HEAP=1"). Empty by default: the
# gauntlet's job is to exercise what ships.
if ! make clean-kernel $GAUNTLET_MAKEFLAGS iso disk-image > "$WORK/build.log" 2>&1; then
```

and add near the other defaults at the top:

```bash
GAUNTLET_MAKEFLAGS=${GAUNTLET_MAKEFLAGS:-}
```

Add the heap panics to the never-retry list, since they are real bugs
by construction. Extend `HARD_RE` with:

```
|heap\] PANIC
```

- [ ] **Step 4: Add per-marker flakiness reporting**

`check_log` already emits `missing: <marker>` lines. Accumulate them
across runs. Before the `for i in $(seq 1 "$N")` reporting loop, add:

```bash
MISSES=$WORK/misses.txt
: > "$MISSES"
```

inside that loop, after `out=$(check_log ...)`, add:

```bash
  grep '^missing: ' <<<"$out" | sed 's/^missing: //' >> "$MISSES"
```

and before the final `echo "---"`, add:

```bash
if [ -s "$MISSES" ]; then
  echo "--- per-marker flakiness over $N run(s) ---"
  sort "$MISSES" | uniq -c | sort -rn | while read -r n m; do
    pct=$(( n * 100 / N ))
    printf '  %3d%%  %2d/%d  %s\n' "$pct" "$n" "$N" "$m"
  done
fi
```

This is the point of the task: a marker that misses 1 run in 15 now
shows as `7% 1/15 [term] render ALL PASSED` instead of vanishing into a
green checkmark.

- [ ] **Step 5: Run it**

```bash
tools/gauntlet.sh 15 3
```

Expected: `PGAUNTLET PASSED: 15/15`, plus a flakiness table if any run
missed a marker before its retry.

- [ ] **Step 6: Run it against the debug heap**

```bash
GAUNTLET_MAKEFLAGS=DEBUG_HEAP=1 tools/gauntlet.sh 15 3
```

Expected: `PGAUNTLET PASSED: 15/15`. **A `[heap] PANIC` here is a real
find, not a gauntlet problem** — it means 15 boots of the existing
kernel contain a use-after-free, double free, or overrun that has been
invisible until now. Report it with the serial log rather than
disabling the check; it is exactly what CS1 was built to surface.

- [ ] **Step 7: Update every reference to the old path**

```bash
grep -rn "pgauntlet.sh" docs/ CLAUDE.md README.md 2>/dev/null
```

Replace each with `tools/gauntlet.sh`. At minimum the CS spec's
constraints section, the roadmap's cross-cutting constraints, and this
plan's own Global Constraints.

- [ ] **Step 8: Commit**

```bash
git add -A tools docs .superpowers
git commit -m "CS1: promote the gauntlet to tools/, report per-marker flakiness"
```

---

## Task 7: Close out CS1

**Files:**
- Modify: `docs/superpowers/specs/2026-09-01-concurrency-and-scaling-design.md` (CS1 section)
- Create: `docs/debugging-tools.md`

- [ ] **Step 1: Write the tool documentation**

Create `docs/debugging-tools.md` covering, for each of the three: what
it catches, how to turn it on, what its output looks like, and what a
failure means. Include the exact panic formats from Tasks 2-4 so a
future reader can recognise one in a serial log, and the `[lockstat]`
column meanings from Task 5. State plainly that all three are
debug-build-only and absent from a shipping kernel.

- [ ] **Step 2: Record the CS1 result in the spec**

Mark CS1 done in the spec's CS1 section. Record what the debug-heap
gauntlet run in Task 6 Step 6 found — including "nothing", which is
itself the useful baseline for CS2. Note the redzone's one real
limitation: it covers size-class allocations only, because a large
allocation's memory is handed straight back to the pmm on free and
there is nothing left to check.

- [ ] **Step 3: Final verification, all three builds**

```bash
make clean-kernel && make test 2>&1 | tail -2
make clean-kernel && make DEBUG_HEAP=1 test 2>&1 | tail -2
make clean-kernel && make DEBUG_LOCKSTAT=1 test 2>&1 | tail -2
tools/gauntlet.sh 15 3
```

Expected: `PASS` three times and `PGAUNTLET PASSED: 15/15`.

- [ ] **Step 4: Commit**

```bash
git add docs
git commit -m "CS1 done; document the debug heap, lockstat, and the gauntlet"
```

---

## Notes for the executor

- **Task order is load-bearing for 2 → 3 → 4.** Task 3 uses Task 2's
  magic; Task 4 checks its redzone before Task 3's poisoning overwrites
  it, so the checks must be inserted in that order inside
  `kfree_locked`. Tasks 5 and 6 are independent of the heap work.
- **Every detector gets proved by making it fire**, then the proof is
  reverted. Steps 5/4/4 of Tasks 2/3/4 exist for that reason. A
  detector that has only ever been green is not evidence.
- **`serial_write_hex64` is the only number formatter available** in
  the kernel — there is no `%d`. All the panic messages above print
  hex, and the plan's expected values are written in hex to match.
- **If the debug-heap gauntlet finds a real bug (Task 6 Step 6), stop
  and report it.** Do not fold a bug fix into CS1 — it belongs in CS2
  with its own regression test, and CS1's job is to have found it.
