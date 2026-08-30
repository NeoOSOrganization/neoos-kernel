# SMP & Concurrency (Phase 10) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Boot NeoOS on four CPUs, with every shared kernel subsystem
protected by a rank-checked fine-grained lock, work-stealing per-CPU run
queues, and reschedule / TLB-shootdown / panic-stop IPIs.

**Architecture:** Application processors are started one at a time via
INIT-SIPI-SIPI into a trampoline copied to physical `0x8000`, which
adopts the boot page tables (the low 4 GiB identity map is permanent)
and calls into the higher-half kernel. Before any AP is allowed to run,
the six subsystems whose lock ranks were declared but never implemented
(`pmm`, `heap`, `runqueue`, `vfs`, `paging`/`vma`, `ata`) plus `waitq`
get real locks, added in blast-radius order so each step boots on its
own.

**Tech Stack:** C (gnu11, freestanding, `-mcmodel=kernel`), NASM,
`x86_64-elf-gcc` cross toolchain, QEMU with `-smp 4`, GRUB/Multiboot2.

**Spec:** `docs/superpowers/specs/2026-08-30-smp-milestone-design.md`

## Global Constraints

- **No host test runner exists.** This is bare-metal code with no host
  runtime. Every test is a kernel selftest whose result is a line on the
  serial port, verified by running QEMU headless and grepping the log.
  Do not add a host unit-test framework.
- **Work happens directly on `main`.** No feature branches.
- Build flags are fixed by the Makefile: `-ffreestanding
  -fno-stack-protector -mno-red-zone -mno-mmx -mno-sse -mno-sse2
  -mcmodel=kernel -Wall -Wextra -std=gnu11 -O2`. **The kernel is
  compiled without SSE** — do not use floating point in kernel code.
- **Selftest output convention**, matched exactly so the log grep works:
  success is `[subsystem] selftest passed`, failure is
  `[subsystem] selftest FAILED: <reason>`. A selftest returns early on
  the first failure.
- **Lock rank rule:** acquisition must be *strictly ascending*. Equal
  ranks are an inversion. Rank 0 is outermost.
- **`schedule()` must be entered holding zero spinlocks.**
- **A frame is returned to pmm only after every TLB-shootdown
  acknowledgement is in.**
- `MAX_CPUS` is 8; QEMU runs with `-smp 4`.
- **`cpus[]` is indexed by dense CPU index, never by `lapic_id`.**
- Any kernel feature reachable from userland needs a musl shim entry or
  `lib/` wrapper plus a `docs/stdlib.md` update (CLAUDE.md).
- Userland-observable shapes must be Linux-shaped; deliberate
  divergences get recorded (CLAUDE.md, Linux ABI compatibility).

---

## File Structure

**Created:**

| File | Responsibility |
|------|----------------|
| `kernel/smp.c` / `.h` | CPU topology table, AP bringup, IPI send/receive |
| `kernel/ap_trampoline.asm` | 16→32→64-bit AP entry, copied to `0x8000` |
| `kernel/smp_selftest.c` | The three SMP selftests (parallel, steal, shootdown) |
| `kernel/tlb.c` / `.h` | Shootdown descriptors and the deferred-free queue |
| `docs/abi-compatibility.md` | Closing deliverable |

**Modified:** `Makefile` (headless test target, `-smp 4`, new objects),
`kernel/acpi.{c,h}` (MADT type 0), `kernel/lock.{c,h}` (ranks,
ordered-pair, `schedule` assertion), `kernel/cpu_local.{c,h}`,
`kernel/tss.{c,h}`, `kernel/gdt.c`, `kernel/mm/pmm.c`,
`kernel/mm/heap.c`, `kernel/mm/paging.{c,h}`, `kernel/mm/vma.c`,
`kernel/fs/vfs.c`, `kernel/ata.c`, `kernel/waitq.{c,h}`,
`kernel/sched/sched.c`, `kernel/sched/proc.{c,h}`, `kernel/kernel.c`,
`kernel/syscall.c`, `docs/stdlib.md`, `OPTIMIZATION_SUMMARY.md`.

---

### Task 1: Headless test harness

Nothing in this repo can be verified without a human watching a QEMU
window. Every later task's test cycle depends on this existing first.

**Files:**
- Modify: `Makefile:196-197` (the `run` target)

**Interfaces:**
- Produces: `make test` — boots headless, captures serial to
  `build/serial.log`, exits non-zero if any `FAILED` line appears or if
  the expected end-of-boot marker is missing.

- [ ] **Step 1: Add the headless target**

Add after the existing `run` target. `-display none -serial stdio`
sends the serial port to stdout; `timeout` bounds a hang; `-no-reboot`
plus `-no-shutdown` keep a triple fault from silently rebooting into a
second confusing boot.

```makefile
SMP_CPUS ?= 4
QEMU_COMMON := -cpu Nehalem -smp $(SMP_CPUS) -boot order=d \
	-cdrom $(BUILD_DIR)/neoos.iso \
	-drive file=$(DISK_IMG),format=raw -drive file=$(DISK2_IMG),format=raw \
	-no-reboot -no-shutdown

run: iso disk-image
	qemu-system-x86_64 $(QEMU_COMMON)

# Headless boot with the serial log captured. Fails the build if any
# selftest reported FAILED, or if the boot never reached the marker
# (a hang or triple fault). BOOT_TIMEOUT is generous: bringing up
# APs plus every selftest is slower than a plain boot.
BOOT_TIMEOUT ?= 60
BOOT_MARKER  ?= NeoOS: interrupts enabled, starting scheduler

test: iso disk-image
	@mkdir -p $(BUILD_DIR)
	-timeout $(BOOT_TIMEOUT) qemu-system-x86_64 $(QEMU_COMMON) \
		-display none -serial stdio > $(BUILD_DIR)/serial.log 2>&1
	@echo "--- serial log: $(BUILD_DIR)/serial.log ---"
	@if grep -q 'FAILED' $(BUILD_DIR)/serial.log; then \
		echo "TEST FAILURES:"; grep 'FAILED' $(BUILD_DIR)/serial.log; exit 1; fi
	@if ! grep -q '$(BOOT_MARKER)' $(BUILD_DIR)/serial.log; then \
		echo "BOOT DID NOT COMPLETE (no marker: '$(BOOT_MARKER)')"; \
		tail -30 $(BUILD_DIR)/serial.log; exit 1; fi
	@echo "PASS: no FAILED lines, boot reached the scheduler"
```

Add `test` to the `.PHONY` line:

```makefile
.PHONY: all build iso run test clean disk-image
```

- [ ] **Step 2: Verify the harness catches a real failure**

Temporarily break a selftest so the harness has something to catch.
In `kernel/mm/pmm.c`, inside `pmm_selftest()`, add as the first line:

```c
serial_write_string("[pmm] selftest FAILED: harness check\n");
```

Run: `make test`
Expected: exits non-zero, prints `TEST FAILURES:` and the injected line.
**If it exits zero, the harness is broken — fix it before continuing.**

- [ ] **Step 3: Remove the injected failure**

Delete the line added in Step 2.

Run: `make test`
Expected: exits zero, prints
`PASS: no FAILED lines, boot reached the scheduler`.

Note: this runs `-smp 4` already, but the kernel still ignores every CPU
but the first. That is expected until Task 12.

- [ ] **Step 4: Commit**

```bash
git add Makefile
git commit -m "Add headless make test: serial capture, FAILED grep, boot marker"
```

---

### Task 2: MADT Local APIC parsing and the CPU topology table

**Files:**
- Modify: `kernel/acpi.h`, `kernel/acpi.c:134-176`
- Create: `kernel/smp.h`, `kernel/smp.c`
- Modify: `kernel/kernel.c:61`, `Makefile`

**Interfaces:**
- Consumes: `acpi_find_madt(struct acpi_info *)` (existing).
- Produces:
  - `struct acpi_cpu { uint8_t acpi_id; uint8_t lapic_id; uint8_t usable; }`
  - `info->cpus[ACPI_MAX_CPUS]`, `info->cpu_count`
  - `void smp_topology_init(const struct acpi_info *info);`
  - `int smp_cpu_count(void);` — CPUs discovered (not yet online)
  - `int smp_index_for_lapic(uint32_t lapic_id);` — dense index, or `-1`
  - `uint32_t smp_lapic_for_index(int index);`

- [ ] **Step 1: Write the failing selftest**

Create `kernel/smp.c` with only the selftest for now, so it fails
against the not-yet-written parser.

```c
// kernel/smp.c -- CPU topology, application-processor bringup, IPIs.
#include "smp.h"
#include "acpi.h"
#include "serial.h"

void smp_topology_selftest(void) {
    int n = smp_cpu_count();
    // QEMU is launched with -smp 4 (see the Makefile). Discovering
    // fewer means the MADT walk is dropping Local APIC entries.
    if (n < 2) {
        serial_write_string("[smp] selftest FAILED: fewer than 2 CPUs discovered\n");
        return;
    }
    // Dense indices must round-trip through the lapic_id lookup, since
    // every IPI target is resolved that way.
    for (int i = 0; i < n; i++) {
        uint32_t lapic = smp_lapic_for_index(i);
        if (smp_index_for_lapic(lapic) != i) {
            serial_write_string("[smp] selftest FAILED: lapic_id round-trip\n");
            return;
        }
    }
    if (smp_index_for_lapic(0xDEAD) != -1) {
        serial_write_string("[smp] selftest FAILED: unknown lapic_id not rejected\n");
        return;
    }
    serial_write_string("[smp] topology selftest passed\n");
}
```

- [ ] **Step 2: Add the header**

Create `kernel/smp.h`:

```c
#ifndef NEOOS_SMP_H
#define NEOOS_SMP_H

#include <stdint.h>
#include "acpi.h"

void     smp_topology_init(const struct acpi_info *info);
int      smp_cpu_count(void);
int      smp_index_for_lapic(uint32_t lapic_id);
uint32_t smp_lapic_for_index(int index);
void     smp_topology_selftest(void);

#endif
```

- [ ] **Step 3: Extend the ACPI info struct**

In `kernel/acpi.h`, above `struct acpi_info`:

```c
#define ACPI_MAX_CPUS 8

struct acpi_cpu {
    uint8_t acpi_id;
    uint8_t lapic_id;
    uint8_t usable;   // Enabled, or Online Capable
};
```

and add to `struct acpi_info`:

```c
    struct acpi_cpu cpus[ACPI_MAX_CPUS];
    uint32_t        cpu_count;
```

- [ ] **Step 4: Run to verify it fails**

Wire the selftest in temporarily: in `kernel/kernel.c`, right after
`acpi_find_madt(&acpi);`, add `smp_topology_selftest();` and
`#include "smp.h"`. Add `$(BUILD_DIR)/smp.o` to the build by confirming
`kernel/*.c` is already globbed by `C_SOURCES` (it is — no Makefile
change needed for `.c` files in `kernel/`).

Run: `make test`
Expected: FAIL —
`[smp] selftest FAILED: fewer than 2 CPUs discovered`
(`smp_cpu_count()` returns 0 because nothing populates the table yet).

- [ ] **Step 5: Parse Local APIC entries**

In `kernel/acpi.c`, add the entry struct beside the existing
`madt_ioapic`:

```c
// MADT type 0. `flags` bit 0 = Enabled, bit 1 = Online Capable;
// either one means the OS may start this CPU.
struct madt_lapic {
    struct madt_entry_header header;
    uint8_t  acpi_processor_id;
    uint8_t  apic_id;
    uint32_t flags;
} __attribute__((packed));
```

In `parse_madt`, initialise `info->cpu_count = 0;` alongside the other
field initialisers, and add the type-0 branch **before** the existing
`if (entry->type == 1)`:

```c
        if (entry->type == 0) {
            struct madt_lapic *lapic = (struct madt_lapic *)ptr;
            if ((lapic->flags & 0x3) && info->cpu_count < ACPI_MAX_CPUS) {
                struct acpi_cpu *c = &info->cpus[info->cpu_count++];
                c->acpi_id  = lapic->acpi_processor_id;
                c->lapic_id = lapic->apic_id;
                c->usable   = 1;
            }
        } else if (entry->type == 1) {
```

(the existing `else if (entry->type == 2)` chain follows unchanged).

Add to the log line at the end of `acpi_find_madt`:

```c
    serial_write_string("[acpi] cpus="); serial_write_hex64(info->cpu_count);
    serial_write_string("\n");
```

- [ ] **Step 6: Implement the topology table**

Add to `kernel/smp.c`, above the selftest:

```c
// Dense CPU index -> lapic id. cpus[] elsewhere in the kernel is
// indexed the same way. NEVER index anything by lapic_id directly:
// APIC ids are sparse and can exceed MAX_CPUS on real hardware.
static uint32_t lapic_ids[ACPI_MAX_CPUS];
static int      cpu_count;

void smp_topology_init(const struct acpi_info *info) {
    cpu_count = 0;
    for (uint32_t i = 0; i < info->cpu_count && cpu_count < ACPI_MAX_CPUS; i++) {
        if (info->cpus[i].usable) {
            lapic_ids[cpu_count++] = info->cpus[i].lapic_id;
        }
    }
    serial_write_string("[smp] topology: cpus=");
    serial_write_hex64((uint64_t)cpu_count);
    serial_write_string("\n");
}

int smp_cpu_count(void) { return cpu_count; }

int smp_index_for_lapic(uint32_t lapic_id) {
    for (int i = 0; i < cpu_count; i++) {
        if (lapic_ids[i] == lapic_id) { return i; }
    }
    return -1;
}

uint32_t smp_lapic_for_index(int index) {
    if (index < 0 || index >= cpu_count) { return 0xFFFFFFFFu; }
    return lapic_ids[index];
}
```

- [ ] **Step 7: Call it from kmain**

In `kernel/kernel.c`, after `acpi_find_madt(&acpi);`:

```c
    smp_topology_init(&acpi);
    smp_topology_selftest();
```

- [ ] **Step 8: Run to verify it passes**

Run: `make test`
Expected: PASS, log contains `[smp] topology: cpus=0x4` and
`[smp] topology selftest passed`.

- [ ] **Step 9: Commit**

```bash
git add kernel/smp.c kernel/smp.h kernel/acpi.c kernel/acpi.h kernel/kernel.c
git commit -m "Phase 10: discover CPUs from the MADT's Local APIC entries"
```

---

### Task 3: Lock rank table, the schedule() invariant, and ordered pairs

Pure lock-infrastructure change, no new locks yet. Doing it first means
Tasks 4–10 can each use their final rank.

**Files:**
- Modify: `kernel/lock.h:26-41`, `kernel/lock.c`, `kernel/sched/sched.c`

**Interfaces:**
- Produces:
  - `LOCK_RANK_MM` (3), `LOCK_RANK_WAITQ` (9), renumbered ranks
  - `uint64_t spin_lock_ordered_pair(struct spinlock *a, struct spinlock *b);`
  - `void spin_unlock_ordered_pair(struct spinlock *a, struct spinlock *b, uint64_t flags);`

- [ ] **Step 1: Write the failing test**

Extend `lock_selftest()` in `kernel/lock.c`, just before the final
`serial_write_string("[lock] selftest passed\n");`:

```c
    // Two locks of EQUAL rank are an inversion for the normal path,
    // and must stay one -- work stealing is the only caller allowed to
    // hold two run queues, and it goes through the ordered-pair helper.
    struct spinlock qa, qb;
    spin_init(&qa, LOCK_RANK_RUNQUEUE, "selftest-queue-a");
    spin_init(&qb, LOCK_RANK_RUNQUEUE, "selftest-queue-b");

    uint64_t fp = spin_lock_ordered_pair(&qa, &qb);
    if (lock_held_depth() != 2) {
        serial_write_string("[lock] selftest FAILED: ordered pair depth\n");
        return;
    }
    if (qa.locked != 1 || qb.locked != 1) {
        serial_write_string("[lock] selftest FAILED: ordered pair did not lock both\n");
        return;
    }
    spin_unlock_ordered_pair(&qa, &qb, fp);
    if (lock_held_depth() != 0 || qa.locked != 0 || qb.locked != 0) {
        serial_write_string("[lock] selftest FAILED: ordered pair not released\n");
        return;
    }
```

- [ ] **Step 2: Run to verify it fails**

Run: `make test`
Expected: FAIL — the build fails with
`implicit declaration of function 'spin_lock_ordered_pair'`. A build
failure is a valid red state here; the function does not exist yet.

- [ ] **Step 3: Renumber the ranks**

Replace the rank block in `kernel/lock.h`:

```c
#define LOCK_RANK_PROCTABLE   0
#define LOCK_RANK_PROCESS     1
#define LOCK_RANK_THREAD      2
// Per-process address space (vma list + pml4). Sits LOW, not up beside
// HEAP where its allocation behaviour would suggest: a demand-paging
// fault takes this lock and then reads through the filesystem, so it
// must be above nothing in VFS -- and such a fault can sleep, so it
// must be below RUNQUEUE. Next to HEAP it inverts on the first
// mmap-backed page fault.
#define LOCK_RANK_MM          3
#define LOCK_RANK_MOUNTTABLE  4
#define LOCK_RANK_VNODEHASH   5
#define LOCK_RANK_VNODE       6
#define LOCK_RANK_BLOCKDEV    7
#define LOCK_RANK_DRIVER      8
// Per-wait-queue. Above every lock legally held across waitq_sleep()
// (a mutex passes its own guard in as `release`, carrying the mutex's
// rank), and below RUNQUEUE, since the sleep path reaches schedule()
// after the queue is updated.
#define LOCK_RANK_WAITQ       9
#define LOCK_RANK_RUNQUEUE   10
#define LOCK_RANK_FDTABLE    11
#define LOCK_RANK_HEAP       12
#define LOCK_RANK_PMM        13
#define LOCK_RANK_SIGQUEUE   14
#define LOCK_RANK_SERIAL    255
```

- [ ] **Step 4: Implement the ordered pair**

Add to `kernel/lock.h` after `spin_unlock_irqrestore`:

```c
// Acquires two locks of the SAME rank. Only legal for the work-stealing
// path, which must hold two run queues at once. Equal ranks are an
// inversion for the normal checker and stay one; safety here comes from
// a consistent global order instead -- both locks are taken in address
// order, so no two CPUs can build a cycle. `a` and `b` must differ.
uint64_t spin_lock_ordered_pair(struct spinlock *a, struct spinlock *b);
void     spin_unlock_ordered_pair(struct spinlock *a, struct spinlock *b,
                                  uint64_t flags);
```

Add to `kernel/lock.c`:

```c
// Address order is the total order. The spec describes this as
// "CPU-index order"; per-CPU run queue locks live inside cpus[], so
// address order and index order are the same order -- and address
// order also stays correct for any other same-rank pair.
uint64_t spin_lock_ordered_pair(struct spinlock *a, struct spinlock *b) {
    if (a == b) { lock_panic("ordered pair given one lock twice", a->name, 0); }
    struct spinlock *first  = (a < b) ? a : b;
    struct spinlock *second = (a < b) ? b : a;

    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) :: "memory");

    if (!lock_rank_ok(first->rank)) {
        lock_panic("rank inversion", first->name, "see previous acquire");
    }
    struct cpu *c = this_cpu();
    if (c->held_depth + 2 > LOCK_MAX_HELD) {
        lock_panic("held-lock stack overflow", first->name, 0);
    }

    while (__atomic_exchange_n(&first->locked, 1u, __ATOMIC_ACQUIRE)) {
        __asm__ volatile ("pause");
    }
    while (__atomic_exchange_n(&second->locked, 1u, __ATOMIC_ACQUIRE)) {
        __asm__ volatile ("pause");
    }
    // Both ranks are recorded, so the checker still sees the true depth
    // and still rejects a third lock of this rank.
    c->held_ranks[c->held_depth++] = first->rank;
    c->held_ranks[c->held_depth++] = second->rank;
    return flags;
}

void spin_unlock_ordered_pair(struct spinlock *a, struct spinlock *b,
                              uint64_t flags) {
    struct cpu *c = this_cpu();
    if (c->held_depth < 2) {
        lock_panic("ordered-pair unlock with <2 held", a->name, 0);
    }
    c->held_depth -= 2;
    // Release order does not matter for correctness; reverse of
    // acquisition by convention.
    struct spinlock *first  = (a < b) ? a : b;
    struct spinlock *second = (a < b) ? b : a;
    __atomic_store_n(&second->locked, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&first->locked,  0u, __ATOMIC_RELEASE);
    if (flags & (1ULL << 9)) {
        __asm__ volatile ("sti");
    }
}
```

- [ ] **Step 5: Enforce the schedule() invariant**

`waitq_sleep` already releases the caller's guard before calling
`schedule()` (`kernel/waitq.c:63`), so this holds today by accident.
Make it enforced. At the very top of `schedule()` in
`kernel/sched/sched.c`, before the `pushfq`:

```c
    // Entering schedule() with a spinlock held deadlocks every other
    // CPU the moment SMP is real: the lock is released only when this
    // thread runs again, and this thread runs again only when some
    // other CPU makes progress. On one CPU it silently "works", which
    // is exactly why it needs an assertion rather than a comment.
    if (lock_held_depth() != 0) {
        lock_panic("schedule() with a spinlock held", "schedule", 0);
    }
```

- [ ] **Step 6: Run to verify it passes**

Run: `make test`
Expected: PASS, log contains `[lock] selftest passed` and no
`schedule() with a spinlock held` panic anywhere in the boot.

- [ ] **Step 7: Commit**

```bash
git add kernel/lock.c kernel/lock.h kernel/sched/sched.c
git commit -m "Phase 10: add MM and WAITQ ranks, ordered-pair acquire, schedule() lock assertion"
```

---

### Task 4: Lock the physical memory manager

First of the six layers, in blast-radius order. `pmm` is a leaf: it
allocates nothing beneath itself.

**Files:**
- Modify: `kernel/mm/pmm.c`

**Interfaces:**
- Consumes: `LOCK_RANK_PMM` (Task 3).
- Produces: no signature changes. `pmm_alloc`, `pmm_free`,
  `pmm_frame_share`, `pmm_frame_refcount`, `pmm_free_frame_count`
  become internally locked.

- [ ] **Step 1: Write the failing test**

Add to the end of `pmm_selftest()` in `kernel/mm/pmm.c`, before its
final passing message:

```c
    // The lock must exist, and taking it must be legal from a context
    // holding nothing. lock_rank_ok() checks legality WITHOUT
    // acquiring, so this never risks the panic path.
    if (pmm_lock.rank != LOCK_RANK_PMM) {
        serial_write_string("[pmm] selftest FAILED: lock rank wrong\n");
        return;
    }
    if (!lock_rank_ok(LOCK_RANK_PMM)) {
        serial_write_string("[pmm] selftest FAILED: pmm rank not acquirable\n");
        return;
    }
    // An allocation must leave nothing held -- a leaked lock here
    // deadlocks the next allocator call on any CPU.
    uint64_t probe = pmm_alloc(0);
    if (lock_held_depth() != 0) {
        serial_write_string("[pmm] selftest FAILED: alloc leaked the lock\n");
        return;
    }
    pmm_free(probe, 0);
    if (lock_held_depth() != 0) {
        serial_write_string("[pmm] selftest FAILED: free leaked the lock\n");
        return;
    }
```

- [ ] **Step 2: Run to verify it fails**

Run: `make test`
Expected: FAIL — build error, `'pmm_lock' undeclared`.

- [ ] **Step 3: Add the lock**

In `kernel/mm/pmm.c`, add `#include "../lock.h"` and
`#include "../serial.h"` if not already present, then beside
`free_lists`:

```c
// One lock over the whole buddy allocator. pmm is a leaf: it calls
// nothing that takes another lock, so a single lock costs nothing in
// ordering complexity. Non-static only so pmm_selftest can assert its
// rank.
struct spinlock pmm_lock;
```

In `pmm_init`, before the free lists are populated (they must not be
touched before the lock is initialised):

```c
    spin_init(&pmm_lock, LOCK_RANK_PMM, "pmm");
```

- [ ] **Step 4: Wrap the public entry points**

The rule: **lock only in the public functions**, never in the static
helpers (`list_push`, `list_remove`, `add_region`) — those are called
from within already-locked regions, and locking them too would be a
same-rank self-deadlock.

`pmm_alloc` — take the lock at entry, release at **every** return. The
function has an early `return 0` for out-of-memory; that path must
release too:

```c
uint64_t pmm_alloc(unsigned order) {
    uint64_t flags = spin_lock_irqsave(&pmm_lock);
    /* ... existing body unchanged, EXCEPT every `return X;` becomes:
           spin_unlock_irqrestore(&pmm_lock, flags); return X;   */
}
```

Apply the same transformation to `pmm_free`, `pmm_frame_share`,
`pmm_frame_refcount`, and `pmm_free_frame_count`.

`pmm_init` is **not** locked — it runs before any other CPU exists and
before the lock is meaningful.

`pmm_selftest` is not locked either, but note it calls `pmm_alloc`,
which now locks; that is why the selftest checks `lock_held_depth()`
is back to 0 afterwards.

- [ ] **Step 5: Run to verify it passes**

Run: `make test`
Expected: PASS, `[pmm] selftest passed`, and every other selftest still
passes (heap and paging both allocate through pmm, so a leaked lock
would hang the boot before the marker).

- [ ] **Step 6: Commit**

```bash
git add kernel/mm/pmm.c
git commit -m "Phase 10: lock the physical memory manager"
```

---

### Task 5: Lock the heap

**Files:**
- Modify: `kernel/mm/heap.c`

**Interfaces:**
- Consumes: `LOCK_RANK_HEAP`; locked pmm from Task 4.
- Produces: `kmalloc`/`kfree` internally locked, taking `pmm_lock`
  beneath `heap_lock` — the already-declared order (12 then 13).

- [ ] **Step 1: Write the failing test**

Add to the end of `heap_selftest()` in `kernel/mm/heap.c`, before its
passing message:

```c
    if (heap_lock.rank != LOCK_RANK_HEAP) {
        serial_write_string("[heap] selftest FAILED: lock rank wrong\n");
        return;
    }
    // The heap calls pmm when it needs more pages, so PMM must be
    // legally acquirable while HEAP is held -- strictly ascending.
    uint64_t f = spin_lock_irqsave(&heap_lock);
    int pmm_ok = lock_rank_ok(LOCK_RANK_PMM);
    spin_unlock_irqrestore(&heap_lock, f);
    if (!pmm_ok) {
        serial_write_string("[heap] selftest FAILED: pmm not acquirable under heap\n");
        return;
    }
    void *probe = kmalloc(64);
    if (lock_held_depth() != 0) {
        serial_write_string("[heap] selftest FAILED: kmalloc leaked the lock\n");
        return;
    }
    kfree(probe);
    if (lock_held_depth() != 0) {
        serial_write_string("[heap] selftest FAILED: kfree leaked the lock\n");
        return;
    }
```

- [ ] **Step 2: Run to verify it fails**

Run: `make test`
Expected: FAIL — build error, `'heap_lock' undeclared`.

- [ ] **Step 3: Add the lock and wrap the entry points**

In `kernel/mm/heap.c`, add `#include "../lock.h"` if absent and declare:

```c
// One lock over the heap's free lists. Takes pmm_lock beneath it when
// it needs more pages -- HEAP (12) then PMM (13) is ascending, which
// is the order the rank table has always declared.
struct spinlock heap_lock;
```

In `heap_init`, as its first statement:

```c
    spin_init(&heap_lock, LOCK_RANK_HEAP, "heap");
```

Wrap `kmalloc`, `kfree`, and any other public allocation entry point in
the file (e.g. `krealloc`, `kcalloc` — wrap whichever exist) with
`spin_lock_irqsave(&heap_lock)` / `spin_unlock_irqrestore`, releasing on
**every** return path including error returns. Do not lock static
helpers.

**Do not** hold `heap_lock` across a `serial_write_string` call in an
error path — serial is rank 255 and legal to take, but keeping the
window short matters on four CPUs.

- [ ] **Step 4: Run to verify it passes**

Run: `make test`
Expected: PASS, `[heap] selftest passed`.

- [ ] **Step 5: Commit**

```bash
git add kernel/mm/heap.c
git commit -m "Phase 10: lock the kernel heap, taking pmm beneath it"
```

---

### Task 6: Lock the per-CPU run queues

Phase 7 left per-CPU queues with no lock at all. This makes
`LOCK_RANK_RUNQUEUE` real. Still single-CPU at this point, so the lock
is uncontended — but every path is in place before an AP arrives.

**Files:**
- Modify: `kernel/cpu_local.h:25-39`, `kernel/cpu_local.c`,
  `kernel/sched/sched.c:26-70`

**Interfaces:**
- Consumes: `LOCK_RANK_RUNQUEUE`, `spin_lock_ordered_pair` (Task 3).
- Produces:
  - `struct cpu` gains `struct spinlock ready_lock;` and `uint32_t ready_count;`
  - `void enqueue_ready_on(int cpu_index, struct thread *t);`
  - `enqueue_ready`, `dequeue_ready`, `dequeue_specific` become locked.

- [ ] **Step 1: Write the failing test**

Create the SMP selftest file now; it grows over Tasks 13, 15 and 16.

Create `kernel/smp_selftest.c`:

```c
// kernel/smp_selftest.c -- selftests that only mean anything with more
// than one CPU. Kept out of smp.c so the bringup code stays readable.
#include "smp.h"
#include "cpu_local.h"
#include "lock.h"
#include "serial.h"
#include "sched/proc.h"

void runqueue_lock_selftest(void) {
    struct cpu *c = this_cpu();
    if (c->ready_lock.rank != LOCK_RANK_RUNQUEUE) {
        serial_write_string("[runq] selftest FAILED: lock rank wrong\n");
        return;
    }
    // ready_count must track the queue, since work stealing picks its
    // victim by comparing counts without walking the lists.
    uint64_t f = spin_lock_irqsave(&c->ready_lock);
    uint32_t counted = 0;
    for (struct thread *t = c->ready_head; t; t = t->next) { counted++; }
    uint32_t claimed = c->ready_count;
    spin_unlock_irqrestore(&c->ready_lock, f);

    if (counted != claimed) {
        serial_write_string("[runq] selftest FAILED: ready_count disagrees with the list\n");
        return;
    }
    serial_write_string("[runq] selftest passed\n");
}
```

Declare it in `kernel/smp.h`:

```c
void runqueue_lock_selftest(void);
```

Call it from `kmain` immediately after `process_init();`.

- [ ] **Step 2: Run to verify it fails**

Run: `make test`
Expected: FAIL — build error, `struct cpu has no member named 'ready_lock'`.

- [ ] **Step 3: Extend struct cpu**

In `kernel/cpu_local.h`, replace the Phase 7 queue fields:

```c
    // Per-CPU ready queue. Phase 7 added the layout; Phase 10 added the
    // lock that makes it safe to touch from a stealing CPU.
    struct spinlock   ready_lock;
    struct thread    *ready_head;
    struct thread    *ready_tail;
    uint32_t          ready_count;  // length of the list, for steal victim choice
```

The `CPU_*` byte offsets asserted at the bottom of the header cover
`self` through `kernel_stack` only, and all of those are declared
*above* the queue fields — so appending here does not disturb them.
**Verify this**: the `_Static_assert` block must still compile
unchanged. If it does not, the new fields were inserted too early in
the struct.

- [ ] **Step 4: Initialise the lock**

In `kernel/cpu_local.c`, in `cpu_local_init()`, alongside the other
field initialisers:

```c
    c->ready_head  = 0;
    c->ready_tail  = 0;
    c->ready_count = 0;
    spin_init(&c->ready_lock, LOCK_RANK_RUNQUEUE, "runqueue");
```

- [ ] **Step 5: Lock the queue operations**

In `kernel/sched/sched.c`, replace `enqueue_ready`, `dequeue_ready` and
`dequeue_specific` with locked versions, and add the unlocked inner
helpers that the locked wrappers and (later) the stealing code share:

```c
// Unlocked. Caller must hold c->ready_lock.
static void ready_push(struct cpu *c, struct thread *t) {
    t->next = 0;
    if (c->ready_tail) { c->ready_tail->next = t; } else { c->ready_head = t; }
    c->ready_tail = t;
    c->ready_count++;
}

// Unlocked. Caller must hold c->ready_lock. Returns 0 if empty.
static struct thread *ready_pop(struct cpu *c) {
    struct thread *t = c->ready_head;
    if (t) {
        c->ready_head = t->next;
        if (!c->ready_head) { c->ready_tail = 0; }
        t->next = 0;
        c->ready_count--;
    }
    return t;
}

void enqueue_ready(struct thread *t) {
    struct cpu *c = this_cpu();
    uint64_t f = spin_lock_irqsave(&c->ready_lock);
    ready_push(c, t);
    spin_unlock_irqrestore(&c->ready_lock, f);
}

struct thread *dequeue_ready(void) {
    struct cpu *c = this_cpu();
    uint64_t f = spin_lock_irqsave(&c->ready_lock);
    struct thread *t = ready_pop(c);
    spin_unlock_irqrestore(&c->ready_lock, f);
    return t;
}

void thread_enqueue_ready(struct thread *t) { enqueue_ready(t); }

// Removes `t` from this CPU's ready queue wherever it sits. Only used
// by idle_init, which has to un-enqueue the idle thread that
// thread_alloc_kernel just queued.
void dequeue_specific(struct thread *t) {
    struct cpu *c = this_cpu();
    uint64_t f = spin_lock_irqsave(&c->ready_lock);
    struct thread **pp = &c->ready_head;
    struct thread *prev = 0;
    while (*pp && *pp != t) { prev = *pp; pp = &(*pp)->next; }
    if (*pp) {
        *pp = t->next;
        if (c->ready_tail == t) { c->ready_tail = prev; }
        c->ready_count--;
    }
    t->next = 0;
    spin_unlock_irqrestore(&c->ready_lock, f);
}
```

- [ ] **Step 6: Fix schedule()'s use of the queue**

`schedule()` calls `dequeue_ready()` and `enqueue_ready()`, which now
take the lock. Both calls happen with interrupts already cleared and no
lock held, so they are legal as written — **but the Task 3 assertion at
the top of `schedule()` runs before either**, so no change is needed.
Confirm by reading `schedule()` that no lock is held across either call.

- [ ] **Step 7: Protect the kzombies list**

`kernel/sched/thread.c:143-145` pushes an exiting kernel thread onto the
global `kzombies` list with a bare two-line splice:

```c
        t->proc_next = kzombies;
        kzombies     = t;
```

`idle_entry` (`kernel/sched/sched.c:77-81`) drains that same list under
`proc_lock`. The push does **not** take `proc_lock`. On one CPU the
window is invisible; with four CPUs exiting kernel threads, two pushes
race and one thread's stack leaks forever — or worse, the drain frees a
thread another CPU is still splicing.

Take `proc_lock` around the push, matching the drain:

```c
        uint64_t zf = spin_lock_irqsave(&proc_lock);
        t->proc_next = kzombies;
        kzombies     = t;
        spin_unlock_irqrestore(&proc_lock, zf);
```

**Check the surrounding function first:** if `proc_lock` is already held
at that point, do not take it again — that is a same-rank self-deadlock.
Read from the top of the function before editing.

- [ ] **Step 8: Run to verify it passes**

Run: `make test`
Expected: PASS, `[runq] selftest passed`, and no
`schedule() with a spinlock held` panic.

- [ ] **Step 9: Commit**

```bash
git add kernel/cpu_local.h kernel/cpu_local.c kernel/sched/sched.c kernel/sched/thread.c kernel/smp_selftest.c kernel/smp.h kernel/kernel.c
git commit -m "Phase 10: give the per-CPU ready queues real locks"
```

---

### Task 7: Lock the wait queues

`waitq`'s `head`/`tail` are currently protected only by whichever guard
the caller happens to hold — a *different* lock for different callers.
This is a latent bug, not a missing feature.

**Files:**
- Modify: `kernel/waitq.h`, `kernel/waitq.c`

**Interfaces:**
- Consumes: `LOCK_RANK_WAITQ` (Task 3).
- Produces: `struct waitq` gains `struct spinlock lock;`. `waitq_init`,
  `waitq_sleep`, `waitq_wake_one`, `waitq_wake_all`, `waitq_remove`
  keep their signatures.

- [ ] **Step 1: Write the failing test**

Add to `waitq_selftest` (in `kernel/waitq.c`; if the existing selftest
is started as a thread via `waitq_selftest_start`, add this as a new
synchronous `waitq_lock_selftest()` called from `kmain` instead):

```c
void waitq_lock_selftest(void) {
    struct waitq q;
    waitq_init(&q);
    if (q.lock.rank != LOCK_RANK_WAITQ) {
        serial_write_string("[waitq] selftest FAILED: lock rank wrong\n");
        return;
    }
    // A mutex passes its own guard into waitq_sleep as `release`, so
    // WAITQ must be acquirable while a guard of mutex rank is held.
    struct spinlock guard;
    spin_init(&guard, LOCK_RANK_VNODE, "selftest-guard");
    uint64_t f = spin_lock_irqsave(&guard);
    int ok = lock_rank_ok(LOCK_RANK_WAITQ);
    spin_unlock_irqrestore(&guard, f);
    if (!ok) {
        serial_write_string("[waitq] selftest FAILED: waitq not acquirable under a mutex guard\n");
        return;
    }
    // And RUNQUEUE must be acquirable under WAITQ, since the sleep path
    // reaches schedule() after touching the queue.
    uint64_t f2 = spin_lock_irqsave(&q.lock);
    int rq_ok = lock_rank_ok(LOCK_RANK_RUNQUEUE);
    spin_unlock_irqrestore(&q.lock, f2);
    if (!rq_ok) {
        serial_write_string("[waitq] selftest FAILED: runqueue not acquirable under waitq\n");
        return;
    }
    serial_write_string("[waitq] lock selftest passed\n");
}
```

Declare it in `kernel/waitq.h` and call it from `kmain` after
`lock_selftest();`.

- [ ] **Step 2: Run to verify it fails**

Run: `make test`
Expected: FAIL — build error, `struct waitq has no member named 'lock'`.

- [ ] **Step 3: Add the lock**

In `kernel/waitq.h`, add to `struct waitq`:

```c
    struct spinlock lock;   // protects head/tail; see waitq.c
```

`waitq.h` is included by `lock.h`, so it must not include `lock.h`
back. Forward-declare instead, above `struct waitq`:

```c
struct spinlock;
```

and make the member a pointer-free embedded struct only if `lock.h`'s
definition is visible. **It is not** — so instead move `struct spinlock`'s
*definition* into a new minimal header `kernel/spinlock_types.h`
included by both `lock.h` and `waitq.h`:

```c
#ifndef NEOOS_SPINLOCK_TYPES_H
#define NEOOS_SPINLOCK_TYPES_H

#include <stdint.h>

// Split out of lock.h purely to break the lock.h <-> waitq.h include
// cycle: waitq embeds a spinlock, and lock.h's mutex embeds a waitq.
struct spinlock {
    volatile uint32_t locked;
    uint8_t     rank;
    const char *name;
};

#endif
```

Remove the `struct spinlock` definition from `lock.h` and have it
`#include "spinlock_types.h"` instead. Have `waitq.h` include it too.

- [ ] **Step 4: Lock the operations**

In `kernel/waitq.c`:

`waitq_init` gains
`spin_init(&q->lock, LOCK_RANK_WAITQ, "waitq");`

`waitq_sleep` — the ordering here is the delicate part. The queue lock
must be taken **after** the signal check and released **before**
`schedule()`, because `schedule()` must be entered holding nothing:

```c
int waitq_sleep(struct waitq *q, struct spinlock *release) {
    struct thread *t = current_thread();

    uint64_t own_flags = 0;
    if (!release) {
        __asm__ volatile ("pushfq; pop %0; cli" : "=r"(own_flags) :: "memory");
    }

    if (signal_pending_any(t)) {
        if (!release && (own_flags & (1ULL << 9))) { __asm__ volatile ("sti"); }
        return -EINTR;
    }

    // The queue lock covers only the enqueue. It is released before
    // schedule(), which must be entered holding no spinlock at all --
    // Task 3 installed an assertion that panics otherwise.
    uint64_t qf = spin_lock_irqsave(&q->lock);
    waitq_enqueue(q, t);
    t->blocked_on = q;
    t->state      = THREAD_BLOCKED;
    spin_unlock_irqrestore(&q->lock, qf);

    if (release) { spin_unlock_irqrestore(release, 0); } // deliberately keeps IF off

    schedule();

    t->blocked_on = 0;
    int rc = signal_pending_any(t) ? -EINTR : 0;

    if (release) { (void)spin_lock_irqsave(release); }
    else if (own_flags & (1ULL << 9)) { __asm__ volatile ("sti"); }
    return rc;
}
```

`waitq_remove` must take `t->blocked_on`'s lock around the list walk.
Re-read `blocked_on` **under** the lock and bail if it changed — the
thread may have been woken between the read and the acquire:

```c
void waitq_remove(struct thread *t) {
    struct waitq *q = t->blocked_on;
    if (!q) { return; }
    uint64_t f = spin_lock_irqsave(&q->lock);
    if (t->blocked_on != q) {   // woken while we were acquiring
        spin_unlock_irqrestore(&q->lock, f);
        return;
    }
    struct thread **pp = &q->head;
    struct thread *prev = 0;
    while (*pp && *pp != t) { prev = *pp; pp = &(*pp)->next; }
    if (*pp) {
        *pp = t->next;
        if (q->tail == t) { q->tail = prev; }
    }
    t->next = 0;
    t->blocked_on = 0;
    spin_unlock_irqrestore(&q->lock, f);
}
```

`waitq_wake_one` / `waitq_wake_all` — take `q->lock` around the
dequeue, then **release it before** calling `enqueue_ready`, so the
WAITQ→RUNQUEUE acquisition never nests. Dequeue into a local list
first, unlock, then enqueue each woken thread.

- [ ] **Step 5: Run to verify it passes**

Run: `make test`
Expected: PASS, `[waitq] lock selftest passed`, the existing
`waitq_selftest` thread still passes, and no rank-inversion panic.

- [ ] **Step 6: Commit**

```bash
git add kernel/waitq.c kernel/waitq.h kernel/lock.h kernel/spinlock_types.h kernel/kernel.c
git commit -m "Phase 10: give each wait queue its own lock, replacing the caller's-guard convention"
```

---

### Task 8: Lock the VFS

**Files:**
- Modify: `kernel/fs/vfs.c`, `kernel/fs/vfs.h`, `kernel/fs/vnode_slab.c`

**Interfaces:**
- Consumes: `LOCK_RANK_MOUNTTABLE` (4), `LOCK_RANK_VNODEHASH` (5),
  `LOCK_RANK_VNODE` (6).
- Produces: `struct vnode` gains `struct spinlock lock;`; module-level
  `mount_lock` and a per-bucket `vnode_hash_locks[]`. No public
  signature changes.

- [ ] **Step 1: Write the failing test**

Add to `vfs_selftest()` in `kernel/fs/vfs.c`, before its passing
message:

```c
    if (mount_lock.rank != LOCK_RANK_MOUNTTABLE) {
        serial_write_string("[vfs] selftest FAILED: mount lock rank wrong\n");
        return;
    }
    // The declared descent is MOUNTTABLE -> VNODEHASH -> VNODE, and
    // below those the block layer. Each step must be legal from the one
    // above, or the first path lookup on a second CPU inverts.
    uint64_t f = spin_lock_irqsave(&mount_lock);
    int hash_ok = lock_rank_ok(LOCK_RANK_VNODEHASH);
    spin_unlock_irqrestore(&mount_lock, f);
    if (!hash_ok) {
        serial_write_string("[vfs] selftest FAILED: vnodehash not acquirable under mounttable\n");
        return;
    }
    uint64_t f2 = spin_lock_irqsave(&vnode_hash_locks[0]);
    int vnode_ok = lock_rank_ok(LOCK_RANK_VNODE);
    spin_unlock_irqrestore(&vnode_hash_locks[0], f2);
    if (!vnode_ok) {
        serial_write_string("[vfs] selftest FAILED: vnode not acquirable under vnodehash\n");
        return;
    }
```

- [ ] **Step 2: Run to verify it fails**

Run: `make test`
Expected: FAIL — build error, `'mount_lock' undeclared`.

- [ ] **Step 3: Add the locks**

In `kernel/fs/vfs.c`:

```c
// The mount table is rarely written and read on every path lookup, so
// one lock over the whole table is enough. Rank 4: taken before any
// vnode work, which is what the declared descent
// MOUNTTABLE -> VNODEHASH -> VNODE means.
struct spinlock mount_lock;

// One lock per hash bucket, matching the pattern Phases 3-5 used for
// the process, thread and fd tables.
struct spinlock vnode_hash_locks[VNODE_HASH_BUCKETS];
```

(If `vfs.c` does not already define `VNODE_HASH_BUCKETS`, use whatever
the existing bucket-count constant is named; do not introduce a second
one.)

In `vfs_init`:

```c
    spin_init(&mount_lock, LOCK_RANK_MOUNTTABLE, "mounttable");
    for (int i = 0; i < VNODE_HASH_BUCKETS; i++) {
        spin_init(&vnode_hash_locks[i], LOCK_RANK_VNODEHASH, "vnodehash");
    }
```

In `kernel/fs/vfs.h`, add to `struct vnode`:

```c
    struct spinlock lock;   // rank LOCK_RANK_VNODE; guards this vnode's fields
```

initialised in `vnode_slab_alloc` (`kernel/fs/vnode_slab.c`) with
`spin_init(&v->lock, LOCK_RANK_VNODE, "vnode");` as part of handing out
a slot.

- [ ] **Step 4: Wrap the operations**

- Mount table: `vfs_mount_fs`, `vfs_umount`, and the mount lookup inside
  path resolution take `mount_lock`.
- Vnode hash: every chain insert, remove and lookup takes
  `vnode_hash_locks[bucket]`.
- Per-vnode: `vnode_get`/`vnode_put` refcount changes and any field
  mutation take `v->lock`.
- The slab's own occupancy bitmap (Phase 5c) is touched under the
  bucket lock of the caller; if `vnode_slab.c` has its own free-slot
  scan reachable from two paths, give it `LOCK_RANK_VNODEHASH` too.

**Critical:** `vnode_put` must not call into the filesystem (which
sleeps) while holding `v->lock`. Phase 5c already learned this in
`fd_table.c` — drop the lock, then call the FS. Follow that shape.

- [ ] **Step 5: Run to verify it passes**

Run: `make test`
Expected: PASS. `[vfs] selftest passed`, `[fat16] selftest passed`,
and the `vfstest` userland suite still passes.

- [ ] **Step 6: Commit**

```bash
git add kernel/fs/vfs.c kernel/fs/vfs.h kernel/fs/vnode_slab.c
git commit -m "Phase 10: lock the VFS mount table, vnode hash buckets, and vnodes"
```

---

### Task 9: Per-process address-space lock

**Files:**
- Modify: `kernel/sched/proc.h`, `kernel/sched/proc.c`, `kernel/mm/vma.c`,
  `kernel/mm/paging.c`

**Interfaces:**
- Consumes: `LOCK_RANK_MM` (3).
- Produces: `struct process` gains `struct spinlock mm_lock;`, guarding
  the vma list and this process's PML4.

- [ ] **Step 1: Write the failing test**

Add to `vma_selftest()` in `kernel/mm/vma.c`:

```c
    // MM sits at rank 3: below the VFS ranks, because a demand-paging
    // fault takes it and then reads through the filesystem; and below
    // RUNQUEUE, because such a fault can sleep.
    struct spinlock probe;
    spin_init(&probe, LOCK_RANK_MM, "selftest-mm");
    uint64_t f = spin_lock_irqsave(&probe);
    int fs_ok = lock_rank_ok(LOCK_RANK_MOUNTTABLE);
    int rq_ok = lock_rank_ok(LOCK_RANK_RUNQUEUE);
    int heap_ok = lock_rank_ok(LOCK_RANK_HEAP);
    spin_unlock_irqrestore(&probe, f);
    if (!fs_ok || !rq_ok || !heap_ok) {
        serial_write_string("[vma] selftest FAILED: mm rank does not dominate fs/runqueue/heap\n");
        return;
    }
```

- [ ] **Step 2: Run to verify it fails**

Run: `make test`
Expected: FAIL — build error, `'LOCK_RANK_MM' undeclared`, **unless**
Task 3 is already merged, in which case this step passes trivially and
the real red state is Step 4's. If it passes here, note that and move
on; the rank check is a guard against a later renumbering regression.

- [ ] **Step 3: Add the lock**

In `kernel/sched/proc.h`, add to `struct process`:

```c
    struct spinlock mm_lock;   // rank LOCK_RANK_MM: vma list + pml4_phys
```

Initialise it wherever a `struct process` is created in
`kernel/sched/proc.c` (both the initial process and `fork`'s child):

```c
    spin_init(&p->mm_lock, LOCK_RANK_MM, "mm");
```

- [ ] **Step 4: Wrap address-space mutation**

Take `p->mm_lock` in:
- `vma` list insert / remove / lookup (`kernel/mm/vma.c`)
- `mmap` / `munmap` syscall handlers
- `paging_map_into` / `paging_unmap_from` when they target a *process*
  PML4 (not the kernel's `p4_table`, which is never mutated after boot)
- the page-fault handler's COW path

**Do not** hold `mm_lock` across `schedule()` — a demand-paging fault
that sleeps must drop it first. The Task 3 assertion will panic if this
is violated, which is the intended safety net.

- [ ] **Step 5: Run to verify it passes**

Run: `make test`
Expected: PASS, `[vma] selftest passed`, and the `mmaptest` userland
suite still passes.

- [ ] **Step 6: Commit**

```bash
git add kernel/sched/proc.h kernel/sched/proc.c kernel/mm/vma.c kernel/mm/paging.c
git commit -m "Phase 10: per-process address-space lock at rank MM"
```

---

### Task 10: Lock the ATA driver

PIO port sequences issued from two CPUs interleave into garbage: one
CPU's `outb` to the sector-count register lands between another's
LBA writes and its command byte.

**Files:**
- Modify: `kernel/ata.c`, `kernel/ata.h`

**Interfaces:**
- Consumes: `LOCK_RANK_DRIVER` (8).
- Produces: `ata_read_sectors` / `ata_write_sectors` / `ata_identify`
  internally locked. No signature changes.

- [ ] **Step 1: Write the failing test**

Add to `blkcache_selftest()` in `kernel/fs/blkcache.c` (the block cache
sits directly above the driver, so this is where the ordering matters):

```c
    // BLOCKDEV (7) then DRIVER (8): the cache holds its own lock while
    // calling into the driver on a miss.
    if (ata_lock.rank != LOCK_RANK_DRIVER) {
        serial_write_string("[blkcache] selftest FAILED: ata lock rank wrong\n");
        return;
    }
```

with `#include "../ata.h"` already present (it is).

- [ ] **Step 2: Run to verify it fails**

Run: `make test`
Expected: FAIL — build error, `'ata_lock' undeclared`.

- [ ] **Step 3: Add the lock**

In `kernel/ata.c`:

```c
// One lock over the drive. A PIO command is a SEQUENCE of port writes
// (drive select, sector count, LBA bytes, then the command byte); two
// CPUs interleaving those sequences issue a command neither asked for.
struct spinlock ata_lock;
```

Declare it in `kernel/ata.h` as `extern struct spinlock ata_lock;` so
the blkcache selftest can see it, and add
`#include "lock.h"` there.

Initialise it in whatever `ata_init`-equivalent runs first; if there is
none, initialise lazily at the top of `ata_identify` guarded by a
`static int inited;` — but prefer adding an explicit `ata_init()` called
from `kmain` before `ata_identify`.

Wrap `ata_read_sectors`, `ata_write_sectors`, and `ata_identify` in
`spin_lock_irqsave(&ata_lock)` / `spin_unlock_irqrestore`, releasing on
every return including timeout and error paths.

- [ ] **Step 4: Run to verify it passes**

Run: `make test`
Expected: PASS, `[blkcache] selftest passed`, `[fat16] selftest passed`.

- [ ] **Step 5: Commit**

```bash
git add kernel/ata.c kernel/ata.h kernel/fs/blkcache.c kernel/kernel.c
git commit -m "Phase 10: serialize ATA PIO command sequences behind a driver lock"
```

---

### Task 11: Per-CPU GDT descriptors, TSS array, and cpu_local split

Last preparation step before an AP can run. Still single-CPU after this.

**Files:**
- Modify: `kernel/cpu_local.h:9`, `kernel/cpu_local.c`, `kernel/tss.h:16`,
  `kernel/tss.c`, `kernel/gdt.c:11-44`, `kernel/gdt.h`

**Interfaces:**
- Produces:
  - `MAX_CPUS` 8, `MAX_TSS` 8
  - `uint16_t gdt_tss_selector(int cpu_index);`
  - `void gdt_load(int cpu_index);` — loads the shared GDT on the
    calling CPU and `ltr`s that CPU's own TSS selector
  - `void cpu_local_init_bsp(void);` / `void cpu_local_init_ap(int index);`

- [ ] **Step 1: Write the failing test**

Add to `kernel/smp_selftest.c`:

```c
void cpu_local_selftest(void) {
    // Every CPU must get a DISTINCT TSS selector: two CPUs sharing one
    // TSS share rsp0, and the second ring-3 entry lands on the first
    // CPU's kernel stack.
    for (int i = 0; i < MAX_CPUS; i++) {
        for (int j = i + 1; j < MAX_CPUS; j++) {
            if (gdt_tss_selector(i) == gdt_tss_selector(j)) {
                serial_write_string("[cpu] selftest FAILED: duplicate TSS selector\n");
                return;
            }
        }
    }
    // The BSP's block must be reachable through GS and self-consistent.
    struct cpu *c = this_cpu();
    if (c->self != c) {
        serial_write_string("[cpu] selftest FAILED: gs:0 does not point at itself\n");
        return;
    }
    if (c->tss != &tss[0]) {
        serial_write_string("[cpu] selftest FAILED: BSP not bound to tss[0]\n");
        return;
    }
    serial_write_string("[cpu] local selftest passed\n");
}
```

Declare in `kernel/smp.h`; call from `kmain` after `cpu_local_init_bsp()`.
Add `#include "tss.h"` and `#include "gdt.h"` to `smp_selftest.c`.

- [ ] **Step 2: Run to verify it fails**

Run: `make test`
Expected: FAIL — build error, `'gdt_tss_selector' undeclared`.

- [ ] **Step 3: Raise the CPU limits**

`kernel/cpu_local.h`: `#define MAX_CPUS 8`
`kernel/tss.h`: `#define MAX_TSS 8`

Confirm `kernel/tss.c` defines `struct tss_entry tss[MAX_TSS];` and that
`tss_init()` initialises all of them (loop over `MAX_TSS`), not just
index 0.

- [ ] **Step 4: One TSS descriptor per CPU in the shared GDT**

A 64-bit TSS descriptor occupies **two** 8-byte slots. Rewrite
`kernel/gdt.c`:

```c
// Layout: 0 null, 1 kernel code, 2 kernel data, 3 user code32,
// 4 user data, 5 user code64, then two slots per CPU for TSS
// descriptors starting at GDT_TSS_FIRST_SLOT.
#define GDT_TSS_FIRST_SLOT 6
static uint64_t gdt_entries[GDT_TSS_FIRST_SLOT + 2 * MAX_CPUS];

uint16_t gdt_tss_selector(int cpu_index) {
    return (uint16_t)((GDT_TSS_FIRST_SLOT + 2 * cpu_index) * 8);
}

static void set_tss_descriptor(int slot, uint64_t base, uint32_t limit) {
    uint64_t low = limit & 0xFFFF;
    low |= (base & 0xFFFFFF) << 16;
    low |= (uint64_t)0x89 << 40;              // present, DPL0, 64-bit TSS available
    low |= (uint64_t)((limit >> 16) & 0xF) << 48;
    low |= ((base >> 24) & 0xFF) << 56;
    gdt_entries[slot]     = low;
    gdt_entries[slot + 1] = (base >> 32) & 0xFFFFFFFF;
}
```

**The user/kernel selector values move**, because the TSS descriptor is
no longer wedged at index 3. `kernel/gdt.h`'s `GDT_*_SELECTOR` macros
and any hard-coded selector in `syscall_entry.asm`, `isr.asm`,
`sigframe.asm` and the IA32_STAR MSR setup in `syscall.c` must be
updated to match the new layout. **Grep for `0x18`, `0x28`, `0x30` and
`0x38` across `kernel/` before changing anything, and update every
site.** Leaving one behind produces a triple fault on the first ring-3
entry, not a build error.

Keep a single `gdt_init()` that fills the table and loads it on the BSP,
plus:

```c
// Loads the SHARED GDT on the calling CPU and installs that CPU's own
// TSS. Every CPU loads the same table and differs only in the selector
// it feeds to ltr.
void gdt_load(int cpu_index) {
    struct gdtr gdtr = { .limit = sizeof(gdt_entries) - 1,
                         .base = (uint64_t)&gdt_entries };
    gdt_flush((uint64_t)&gdtr, GDT_KERNEL_DATA_SELECTOR,
              GDT_KERNEL_CODE_SELECTOR, gdt_tss_selector(cpu_index));
}
```

and have `gdt_init()` populate every CPU's descriptor
(`set_tss_descriptor(GDT_TSS_FIRST_SLOT + 2*i, (uint64_t)&tss[i], ...)`
for `i` in `0..MAX_CPUS`) then call `gdt_load(0)`.

- [ ] **Step 5: Split cpu_local_init**

In `kernel/cpu_local.c`, replace `cpu_local_init` with a shared helper
plus two entry points. Keep the existing GDT-ordering comment on both —
it applies to every CPU:

```c
static void cpu_local_install(int index) {
    struct cpu *c = &cpus[index];
    c->self             = c;
    c->current          = 0;
    c->idle             = 0;
    c->tss              = &tss[index];
    c->user_rsp_scratch = 0;
    c->kernel_stack     = 0;
    c->lapic_id         = 0;
    c->held_depth       = 0;
    c->ready_head       = 0;
    c->ready_tail       = 0;
    c->ready_count      = 0;
    spin_init(&c->ready_lock, LOCK_RANK_RUNQUEUE, "runqueue");

    // Must run AFTER the GDT reload: loading a GS SELECTOR
    // (`mov gs, ax`) zeroes IA32_GS_BASE as a side effect, so any base
    // installed before the GDT reload is silently destroyed. Applies to
    // every CPU, not just the BSP.
    wrmsr64(MSR_GS_BASE, (uint64_t)(uintptr_t)c);
    wrmsr64(MSR_KERNEL_GS_BASE, 0);
}

void cpu_local_init_bsp(void) {
    cpu_local_install(0);
    serial_write_string("[cpu] per-CPU block installed (bsp)\n");
}

void cpu_local_init_ap(int index) {
    cpu_local_install(index);
    cpus[index].lapic_id = lapic_get_id();
}
```

Update `kernel/cpu_local.h`'s declarations and `kmain`'s call site
(`cpu_local_init()` → `cpu_local_init_bsp()`).

- [ ] **Step 6: Run to verify it passes**

Run: `make test`
Expected: PASS, `[cpu] local selftest passed`, and **critically** all
five userland suites still pass — they are the only thing that exercises
the moved ring-3 selectors.

- [ ] **Step 7: Commit**

```bash
git add kernel/cpu_local.c kernel/cpu_local.h kernel/tss.c kernel/tss.h kernel/gdt.c kernel/gdt.h kernel/syscall.c kernel/smp_selftest.c kernel/smp.h kernel/kernel.c
git commit -m "Phase 10: one TSS descriptor per CPU, MAX_CPUS 8, cpu_local BSP/AP split"
```

---

### Task 12: AP bringup — the first multi-CPU boot

**Files:**
- Create: `kernel/ap_trampoline.asm`
- Modify: `kernel/smp.c`, `kernel/smp.h`, `kernel/lapic.c`, `kernel/lapic.h`,
  `kernel/kernel.c`, `Makefile`, `kernel/sched/sched.c` (per-CPU idle)

**Interfaces:**
- Consumes: everything from Tasks 2–11.
- Produces:
  - `void smp_start_aps(void);`
  - `int smp_online_count(void);`
  - `void lapic_send_init(uint32_t lapic_id);`
  - `void lapic_send_sipi(uint32_t lapic_id, uint8_t vector);`
  - `struct cpu` gains `volatile uint32_t online;`
  - `void idle_init_for(int cpu_index);`

- [ ] **Step 1: Write the failing test**

Add to `kernel/smp_selftest.c`:

```c
void smp_online_selftest(void) {
    int discovered = smp_cpu_count();
    int online     = smp_online_count();
    if (online < 2) {
        serial_write_string("[smp] selftest FAILED: no application processor came online\n");
        return;
    }
    if (online > discovered) {
        serial_write_string("[smp] selftest FAILED: more online than discovered\n");
        return;
    }
    // Each online CPU must have its own idle thread and its own TSS.
    for (int i = 0; i < online; i++) {
        if (!cpus[i].idle) {
            serial_write_string("[smp] selftest FAILED: cpu without an idle thread\n");
            return;
        }
        if (cpus[i].tss != &tss[i]) {
            serial_write_string("[smp] selftest FAILED: cpu not bound to its own tss\n");
            return;
        }
    }
    serial_write_string("[smp] online selftest passed\n");
}
```

Declare in `kernel/smp.h`.

- [ ] **Step 2: Run to verify it fails**

Wire the call into `kmain` just before the boot marker line. Run:
`make test`
Expected: FAIL —
`[smp] selftest FAILED: no application processor came online`.
This is the assertion that fails on today's kernel and is the whole
point of the milestone.

- [ ] **Step 3: Add the LAPIC IPI primitives**

In `kernel/lapic.c`, add the ICR registers and senders:

```c
#define LAPIC_REG_ICR_LOW    0x300
#define LAPIC_REG_ICR_HIGH   0x310

#define ICR_DELIVERY_INIT    (5u << 8)
#define ICR_DELIVERY_STARTUP (6u << 8)
#define ICR_DELIVERY_NMI     (4u << 8)
#define ICR_LEVEL_ASSERT     (1u << 14)
#define ICR_TRIGGER_LEVEL    (1u << 15)
#define ICR_DELIVERY_PENDING (1u << 12)

static void lapic_wait_idle(void) {
    while (lapic_read(LAPIC_REG_ICR_LOW) & ICR_DELIVERY_PENDING) {
        __asm__ volatile ("pause");
    }
}

// Writing ICR_HIGH selects the target and ICR_LOW fires the IPI, so
// ICR_HIGH must always be written FIRST.
static void lapic_send_icr(uint32_t lapic_id, uint32_t low) {
    lapic_write(LAPIC_REG_ICR_HIGH, lapic_id << 24);
    lapic_write(LAPIC_REG_ICR_LOW, low);
    lapic_wait_idle();
}

void lapic_send_init(uint32_t lapic_id) {
    lapic_send_icr(lapic_id, ICR_DELIVERY_INIT | ICR_LEVEL_ASSERT | ICR_TRIGGER_LEVEL);
    lapic_send_icr(lapic_id, ICR_DELIVERY_INIT | ICR_TRIGGER_LEVEL); // deassert
}

void lapic_send_sipi(uint32_t lapic_id, uint8_t vector) {
    lapic_send_icr(lapic_id, ICR_DELIVERY_STARTUP | ICR_LEVEL_ASSERT | vector);
}

void lapic_send_ipi(uint32_t lapic_id, uint8_t vector) {
    lapic_send_icr(lapic_id, ICR_LEVEL_ASSERT | vector);
}

void lapic_send_nmi(uint32_t lapic_id) {
    lapic_send_icr(lapic_id, ICR_DELIVERY_NMI | ICR_LEVEL_ASSERT);
}
```

Declare all five in `kernel/lapic.h`.

- [ ] **Step 4: Write the trampoline**

Create `kernel/ap_trampoline.asm`. It is `boot/boot.asm`'s long-mode
path with page-table *construction* removed — the low 4 GiB identity map
built at boot is permanent (`kernel/mm/paging.c` keeps `PML4[0]` because
pmm dereferences through it), so the AP adopts the live `p4_table`.

```nasm
; kernel/ap_trampoline.asm -- application-processor entry.
;
; Copied to physical 0x8000 by smp_start_aps() and entered in 16-bit
; real mode by SIPI. Everything here is position-dependent on 0x8000:
; the SIPI vector IS the page number, so the code cannot be relocated
; without changing AP_TRAMPOLINE_PHYS to match.
;
; pmm never hands out memory below 0x100000 (kernel/mm/pmm.c), so this
; page cannot be allocated out from under us.

AP_BASE equ 0x8000

section .ap_trampoline
[bits 16]
global ap_trampoline_start
global ap_trampoline_end
global ap_trampoline_stack   ; patched by the BSP before each SIPI
global ap_trampoline_index   ; patched by the BSP before each SIPI
global ap_trampoline_cr3     ; patched once, before the first SIPI

ap_trampoline_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    lgdt [AP_BASE + (ap_gdt_pointer - ap_trampoline_start)]

    mov eax, cr0
    or  eax, 1                  ; PE: enter 32-bit protected mode
    mov cr0, eax
    jmp dword 0x08:(AP_BASE + (ap_protected - ap_trampoline_start))

[bits 32]
ap_protected:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; Adopt the BSP's page tables rather than building our own.
    mov eax, [AP_BASE + (ap_trampoline_cr3 - ap_trampoline_start)]
    mov cr3, eax

    mov eax, cr4
    or  eax, 1 << 5             ; PAE
    mov cr4, eax

    mov ecx, 0xC0000080         ; EFER
    rdmsr
    or  eax, 1 << 8             ; LME
    wrmsr

    mov eax, cr0
    or  eax, 1 << 31            ; PG
    mov cr0, eax

    jmp 0x18:(AP_BASE + (ap_long - ap_trampoline_start))

[bits 64]
ap_long:
    mov rsp, [AP_BASE + (ap_trampoline_stack - ap_trampoline_start)]
    mov edi, [AP_BASE + (ap_trampoline_index - ap_trampoline_start)]

    ; ap_main is linked in the higher half; a rel32 call cannot reach
    ; it, exactly as boot.asm documents for kmain.
    mov rax, ap_main
    call rax

.hang:
    cli
    hlt
    jmp .hang

align 8
ap_trampoline_stack: dq 0
ap_trampoline_index: dd 0
ap_trampoline_cr3:   dd 0

align 8
ap_gdt:
    dq 0
    ; 0x08: 32-bit code
    dq 0x00CF9A000000FFFF
    ; 0x10: 32-bit data
    dq 0x00CF92000000FFFF
    ; 0x18: 64-bit code
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53)
ap_gdt_pointer:
    dw ap_gdt_pointer - ap_gdt - 1
    dq AP_BASE + (ap_gdt - ap_trampoline_start)

ap_trampoline_end:

extern ap_main
```

Add to the `Makefile`'s `ASM_OBJECTS` and give it a rule:

```makefile
$(BUILD_DIR)/ap_trampoline.o: kernel/ap_trampoline.asm
	mkdir -p $(BUILD_DIR)
	$(AS) $(ASFLAGS) kernel/ap_trampoline.asm -o $(BUILD_DIR)/ap_trampoline.o
```

and append `$(BUILD_DIR)/ap_trampoline.o` to the `ASM_OBJECTS := ...` line.

- [ ] **Step 5: Per-CPU idle threads**

In `kernel/sched/sched.c`, generalise `idle_init`:

```c
// One idle thread per CPU. The reserved tid is -(index+1) rather than
// 0 so idle threads stay distinguishable from each other and from real
// threads in the serial log.
void idle_init_for(int cpu_index) {
    struct thread *t = thread_alloc_kernel(idle_entry);
    t->tid = -(cpu_index + 1);
    dequeue_specific(t);   // never on the ready queue; schedule() falls back to it
    cpus[cpu_index].idle = t;
}

void idle_init(void) { idle_init_for(0); }
```

Declare `idle_init_for` in `kernel/sched/sched.h`.

Note `dequeue_specific` operates on `this_cpu()`'s queue, and
`thread_alloc_kernel` enqueued onto `this_cpu()` too — so this is
correct as long as each CPU calls `idle_init_for` **for itself**, which
is what `ap_main` does.

- [ ] **Step 6: Implement bringup**

Add to `kernel/smp.c`:

```c
#define AP_TRAMPOLINE_PHYS 0x8000

extern uint8_t ap_trampoline_start[], ap_trampoline_end[];
extern uint8_t ap_trampoline_stack[], ap_trampoline_index[], ap_trampoline_cr3[];

static int online_count = 1;   // the BSP is online by definition

int smp_online_count(void) { return online_count; }

// Rough spin delay. The bringup protocol needs 10ms and 200us waits and
// runs once per boot, so a calibrated timer is not worth it; this only
// has to be AT LEAST the required delay.
static void spin_delay_us(uint64_t us) {
    for (uint64_t i = 0; i < us * 200; i++) { __asm__ volatile ("pause"); }
}

static void *tramp_field(uint8_t *label) {
    return (void *)(uintptr_t)(AP_TRAMPOLINE_PHYS +
                               (uintptr_t)(label - ap_trampoline_start));
}

void ap_main(int index) {
    gdt_load(index);
    idt_load();
    cpu_local_init_ap(index);
    lapic_init_this_cpu();
    cpu_init();
    idle_init_for(index);

    __atomic_store_n(&cpus[index].online, 1u, __ATOMIC_RELEASE);
    __atomic_fetch_add(&online_count, 1, __ATOMIC_ACQ_REL);

    serial_write_string("[smp] cpu online, lapic_id=");
    serial_write_hex64(cpus[index].lapic_id);
    serial_write_string("\n");

    __asm__ volatile ("sti");
    for (;;) { schedule(); __asm__ volatile ("hlt"); }
}

void smp_start_aps(void) {
    uint64_t size = (uint64_t)(ap_trampoline_end - ap_trampoline_start);
    // memcpy to a LOW PHYSICAL address, reachable because the boot
    // identity map covers the first 4GiB and is never torn down.
    uint8_t *dst = (uint8_t *)(uintptr_t)AP_TRAMPOLINE_PHYS;
    for (uint64_t i = 0; i < size; i++) { dst[i] = ap_trampoline_start[i]; }

    *(uint32_t *)tramp_field(ap_trampoline_cr3) = (uint32_t)(uintptr_t)p4_table;

    int total = smp_cpu_count();
    for (int i = 1; i < total && i < MAX_CPUS; i++) {
        uint32_t lapic_id = smp_lapic_for_index(i);

        // A fresh 16KiB kernel stack for the AP to run ap_main on.
        uint64_t stack_phys = pmm_alloc(KERNEL_STACK_ORDER);
        if (!stack_phys) {
            serial_write_string("[smp] no memory for AP stack; skipping cpu\n");
            continue;
        }
        uint64_t stack_top = (uint64_t)(uintptr_t)phys_to_virt(stack_phys)
                           + (PMM_FRAME_SIZE << KERNEL_STACK_ORDER);

        *(uint64_t *)tramp_field(ap_trampoline_stack) = stack_top;
        *(uint32_t *)tramp_field(ap_trampoline_index) = (uint32_t)i;

        cpus[i].online = 0;

        // INIT, then two SIPIs, per the Intel MP protocol.
        lapic_send_init(lapic_id);
        spin_delay_us(10000);
        lapic_send_sipi(lapic_id, AP_TRAMPOLINE_PHYS >> 12);
        spin_delay_us(200);
        lapic_send_sipi(lapic_id, AP_TRAMPOLINE_PHYS >> 12);

        // Bringup is SERIALIZED: wait for this AP before starting the
        // next. All APs share the one trampoline stack slot, so two in
        // flight at once would run on the same stack.
        int waited_us = 0;
        while (!__atomic_load_n(&cpus[i].online, __ATOMIC_ACQUIRE) &&
               waited_us < 100000) {
            spin_delay_us(100);
            waited_us += 100;
        }
        if (!cpus[i].online) {
            // Not fatal: continue with fewer CPUs rather than hanging
            // the boot on one uncooperative core.
            serial_write_string("[smp] cpu failed to come online: lapic_id=");
            serial_write_hex64(lapic_id);
            serial_write_string("\n");
        }
    }

    serial_write_string("[smp] online cpus=");
    serial_write_hex64((uint64_t)online_count);
    serial_write_string("\n");
}
```

Add `volatile uint32_t online;` to `struct cpu` in `kernel/cpu_local.h`.

Split `lapic_init` so an AP can enable its own LAPIC without re-deriving
the MMIO base: keep `lapic_init(address)` for the BSP, and add
`lapic_init_this_cpu(void)` that only writes the SVR (the base is a
shared static already set by the BSP, and the LAPIC MMIO page is
per-CPU-aliased by hardware at the same address). Add `idt_load(void)`
to `kernel/idt.c` that runs `lidt` against the already-built IDT.

- [ ] **Step 7: Call bringup from kmain**

In `kernel/kernel.c`, after the spawns and after
`signal_selftest_start();`, before the boot-marker line:

```c
    smp_start_aps();
    smp_online_selftest();
```

- [ ] **Step 8: Run to verify it passes**

Run: `make test`
Expected: PASS. Log contains three `[smp] cpu online, lapic_id=` lines
(CPUs 1–3), `[smp] online cpus=0x4`, and
`[smp] online selftest passed`.

**If the boot hangs with no `[smp] cpu online` line:** the trampoline
never reached long mode. Check, in order, that (a) the copy landed at
`0x8000`, (b) `ap_trampoline_cr3` was patched before the first SIPI,
(c) every intra-trampoline reference uses the `AP_BASE + (label -
ap_trampoline_start)` form rather than a link-time address.

- [ ] **Step 9: Commit**

```bash
git add kernel/ap_trampoline.asm kernel/smp.c kernel/smp.h kernel/lapic.c kernel/lapic.h kernel/idt.c kernel/idt.h kernel/cpu_local.h kernel/sched/sched.c kernel/sched/sched.h kernel/kernel.c kernel/smp_selftest.c Makefile
git commit -m "Phase 10: bring up application processors via INIT-SIPI-SIPI"
```

---

### Task 13: The parallelism selftest

The test that proves CPUs actually execute work concurrently, rather
than merely reporting themselves online.

**Files:**
- Modify: `kernel/smp_selftest.c`, `kernel/kernel.c`

**Interfaces:**
- Consumes: `smp_online_count()`, locked run queues, `thread_alloc_kernel`.
- Produces: `void smp_parallel_selftest_start(void);`

- [ ] **Step 1: Write the failing test**

Add to `kernel/smp_selftest.c`:

```c
#define PAR_THREADS    8
#define PAR_ITERATIONS 10000

static struct spinlock par_lock;
static uint64_t        par_counter;
static volatile int    par_done;
static volatile uint8_t par_cpu_seen[MAX_CPUS];

static void par_worker(void) {
    for (int i = 0; i < PAR_ITERATIONS; i++) {
        uint64_t f = spin_lock_irqsave(&par_lock);
        par_counter++;
        spin_unlock_irqrestore(&par_lock, f);
        // Recorded outside the lock on purpose: this is about WHERE the
        // work ran, and taking the lock to record it would serialize
        // the very parallelism being measured.
        par_cpu_seen[this_cpu() - &cpus[0]] = 1;
    }
    __atomic_fetch_add((int *)&par_done, 1, __ATOMIC_ACQ_REL);
    thread_exit_self(0);
}

void smp_parallel_selftest_start(void) {
    spin_init(&par_lock, LOCK_RANK_PROCESS, "smp-parallel");
    par_counter = 0;
    par_done    = 0;
    for (int i = 0; i < MAX_CPUS; i++) { par_cpu_seen[i] = 0; }

    for (int i = 0; i < PAR_THREADS; i++) {
        struct thread *t = thread_alloc_kernel(par_worker);
        // Round-robin across online CPUs so the work cannot all land
        // on the BSP by default.
        enqueue_ready_on(i % smp_online_count(), t);
    }
}

// Called from the idle loop once every worker has finished.
void smp_parallel_selftest_check(void) {
    if (__atomic_load_n((int *)&par_done, __ATOMIC_ACQUIRE) != PAR_THREADS) {
        return;   // not finished yet
    }
    static int reported;
    if (reported) { return; }
    reported = 1;

    // A lost increment means the spinlock is not actually mutually
    // exclusive across CPUs.
    if (par_counter != (uint64_t)PAR_THREADS * PAR_ITERATIONS) {
        serial_write_string("[smp] parallel selftest FAILED: counter lost increments\n");
        return;
    }
    int distinct = 0;
    for (int i = 0; i < MAX_CPUS; i++) { if (par_cpu_seen[i]) { distinct++; } }
    // The assertion that fails on the pre-SMP kernel: work must have
    // actually executed on more than one CPU.
    if (distinct < 2) {
        serial_write_string("[smp] parallel selftest FAILED: work ran on only one cpu\n");
        return;
    }
    serial_write_string("[smp] parallel selftest passed\n");
}
```

Add `void enqueue_ready_on(int cpu_index, struct thread *t);` to
`kernel/sched/sched.c`:

```c
// Enqueue onto a SPECIFIC CPU's queue rather than this one. Used at
// thread creation to spread new work, and by the reschedule-IPI path.
void enqueue_ready_on(int cpu_index, struct thread *t) {
    struct cpu *c = &cpus[cpu_index];
    uint64_t f = spin_lock_irqsave(&c->ready_lock);
    ready_push(c, t);
    spin_unlock_irqrestore(&c->ready_lock, f);
}
```

Call `smp_parallel_selftest_start()` from `kmain` after
`smp_online_selftest();`, and `smp_parallel_selftest_check()` from
`idle_entry`'s loop in `kernel/sched/sched.c`.

- [ ] **Step 2: Run to verify it fails on a single-CPU kernel**

Temporarily run with one CPU: `make test SMP_CPUS=1`
Expected: FAIL —
`[smp] parallel selftest FAILED: work ran on only one cpu`.
This is the pre-change red state the Phase 5c lesson requires.

- [ ] **Step 3: Run on four CPUs**

Run: `make test`
Expected: PASS, `[smp] parallel selftest passed`.

If the counter check fails instead, the spinlock is broken across CPUs —
verify `spin_lock_irqsave`'s `__atomic_exchange_n` was not optimised
away and that `par_lock` is not in a per-CPU region.

- [ ] **Step 4: Commit**

```bash
git add kernel/smp_selftest.c kernel/smp.h kernel/sched/sched.c kernel/sched/sched.h kernel/kernel.c
git commit -m "Phase 10: parallelism selftest -- exact counter and >=2 CPUs doing work"
```

---

### Task 14: The reschedule IPI

**Files:**
- Modify: `kernel/smp.c`, `kernel/smp.h`, `kernel/isr.c`, `kernel/sched/sched.c`

**Interfaces:**
- Consumes: `lapic_send_ipi` (Task 12).
- Produces:
  - `#define VECTOR_IPI_RESCHEDULE 0xF0`
  - `void smp_send_reschedule(int cpu_index);`
  - `void ipi_reschedule_handler(void);`

- [ ] **Step 1: Write the failing test**

Add to `kernel/smp_selftest.c`:

```c
void smp_reschedule_ipi_selftest(void) {
    // Target the highest online CPU, which is the most likely to be
    // parked in idle's `sti; hlt`.
    int target = smp_online_count() - 1;
    if (target < 1) {
        serial_write_string("[smp] ipi selftest FAILED: no AP to target\n");
        return;
    }
    uint64_t before = __atomic_load_n(&ipi_reschedule_count, __ATOMIC_ACQUIRE);
    smp_send_reschedule(target);

    // Bounded wait: an IPI that is never delivered must fail the test,
    // not hang the boot.
    for (int i = 0; i < 1000000; i++) {
        if (__atomic_load_n(&ipi_reschedule_count, __ATOMIC_ACQUIRE) > before) {
            serial_write_string("[smp] reschedule ipi selftest passed\n");
            return;
        }
        __asm__ volatile ("pause");
    }
    serial_write_string("[smp] ipi selftest FAILED: reschedule IPI never delivered\n");
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `make test`
Expected: FAIL — build error, `'ipi_reschedule_count' undeclared`.

- [ ] **Step 3: Implement the IPI**

In `kernel/smp.h`:

```c
#define VECTOR_IPI_RESCHEDULE 0xF0

extern volatile uint64_t ipi_reschedule_count;
void smp_send_reschedule(int cpu_index);
void ipi_reschedule_handler(void);
```

In `kernel/smp.c`:

```c
volatile uint64_t ipi_reschedule_count;

// The handler does nothing but acknowledge. Waking the target is the
// INTERRUPT's job -- it breaks the idle loop's `hlt`, and the loop
// calls schedule() on its next pass.
void ipi_reschedule_handler(void) {
    __atomic_fetch_add(&ipi_reschedule_count, 1, __ATOMIC_ACQ_REL);
    lapic_send_eoi();
}

void smp_send_reschedule(int cpu_index) {
    if (cpu_index < 0 || cpu_index >= smp_online_count()) { return; }
    if (cpu_index == (int)(this_cpu() - &cpus[0])) { return; } // no self-IPI needed
    lapic_send_ipi(smp_lapic_for_index(cpu_index), VECTOR_IPI_RESCHEDULE);
}
```

Register the handler in `kernel/isr.c`'s dispatch, beside the existing
`VECTOR_TIMER` and `VECTOR_KEYBOARD` cases:

```c
        case VECTOR_IPI_RESCHEDULE: ipi_reschedule_handler(); break;
```

Call `smp_reschedule_ipi_selftest()` from `kmain` after
`smp_online_selftest();`.

- [ ] **Step 4: Run to verify it passes**

Run: `make test`
Expected: PASS, `[smp] reschedule ipi selftest passed`.

- [ ] **Step 5: Commit**

```bash
git add kernel/smp.c kernel/smp.h kernel/isr.c kernel/smp_selftest.c kernel/kernel.c
git commit -m "Phase 10: reschedule IPI"
```

---

### Task 15: Work stealing

**Files:**
- Modify: `kernel/sched/sched.c`, `kernel/smp_selftest.c`

**Interfaces:**
- Consumes: `spin_lock_ordered_pair` (Task 3), `enqueue_ready_on` (Task 13),
  `smp_send_reschedule` (Task 14).
- Produces: `static struct thread *steal_work(struct cpu *self);`, called
  from `schedule()` when the local queue is empty.

- [ ] **Step 1: Write the failing test**

Add to `kernel/smp_selftest.c`:

```c
#define STEAL_THREADS 16

static volatile int     steal_done;
static volatile uint8_t steal_cpu_seen[MAX_CPUS];

static void steal_worker(void) {
    steal_cpu_seen[this_cpu() - &cpus[0]] = 1;
    __atomic_fetch_add((int *)&steal_done, 1, __ATOMIC_ACQ_REL);
    thread_exit_self(0);
}

void smp_steal_selftest_start(void) {
    steal_done = 0;
    for (int i = 0; i < MAX_CPUS; i++) { steal_cpu_seen[i] = 0; }
    // Deliberately ALL onto CPU 0. Nothing but stealing can spread
    // these, so a passing result proves stealing works.
    for (int i = 0; i < STEAL_THREADS; i++) {
        enqueue_ready_on(0, thread_alloc_kernel(steal_worker));
    }
}

void smp_steal_selftest_check(void) {
    if (__atomic_load_n((int *)&steal_done, __ATOMIC_ACQUIRE) != STEAL_THREADS) {
        return;
    }
    static int reported;
    if (reported) { return; }
    reported = 1;

    int online = smp_online_count();
    for (int i = 0; i < online; i++) {
        if (!steal_cpu_seen[i]) {
            serial_write_string("[smp] steal selftest FAILED: a cpu never stole any work\n");
            return;
        }
    }
    serial_write_string("[smp] steal selftest passed\n");
}
```

Call `smp_steal_selftest_start()` from `kmain` and
`smp_steal_selftest_check()` from `idle_entry`, beside the parallel
test's check.

- [ ] **Step 2: Run to verify it fails**

Run: `make test`
Expected: FAIL —
`[smp] steal selftest FAILED: a cpu never stole any work`. All 16
threads run on CPU 0, because nothing steals yet.

- [ ] **Step 3: Implement stealing**

In `kernel/sched/sched.c`:

```c
// Called only when this CPU's own queue is empty. Picks the busiest
// remote queue and takes ONE thread from its head.
//
// Two run queue locks are held at once, and both are LOCK_RANK_RUNQUEUE
// -- equal ranks, which the checker forbids for good reason. Safety
// comes from a consistent global order instead: spin_lock_ordered_pair
// always acquires in address order, and the queue locks live inside
// cpus[], so address order IS CPU-index order. No two stealing CPUs can
// build a cycle.
static struct thread *steal_work(struct cpu *self) {
    int online = smp_online_count();
    if (online < 2) { return 0; }

    // Pick the victim by count WITHOUT locking. A stale count only
    // means a wasted attempt, never a corrupt list -- the real check
    // happens under the lock below.
    struct cpu *victim = 0;
    uint32_t best = 0;
    for (int i = 0; i < online; i++) {
        struct cpu *c = &cpus[i];
        if (c == self) { continue; }
        uint32_t n = __atomic_load_n(&c->ready_count, __ATOMIC_RELAXED);
        if (n > best) { best = n; victim = c; }
    }
    if (!victim) { return 0; }

    uint64_t f = spin_lock_ordered_pair(&self->ready_lock, &victim->ready_lock);
    struct thread *t = 0;
    // Re-check under the lock: the victim may have been drained between
    // the scan and the acquire.
    if (victim->ready_count > 0) {
        t = ready_pop(victim);
    }
    spin_unlock_ordered_pair(&self->ready_lock, &victim->ready_lock, f);
    return t;
}
```

In `schedule()`, where `dequeue_ready()` returns nothing:

```c
    struct thread *next = dequeue_ready();
    if (!next) {
        next = steal_work(c);
    }
    if (!next) {
        struct thread *cur = c->current;
        /* ... existing fallback unchanged ... */
    }
```

In `enqueue_ready_on`, poke an idle target so a newly-queued thread does
not wait for the next timer tick:

```c
void enqueue_ready_on(int cpu_index, struct thread *t) {
    struct cpu *c = &cpus[cpu_index];
    uint64_t f = spin_lock_irqsave(&c->ready_lock);
    ready_push(c, t);
    spin_unlock_irqrestore(&c->ready_lock, f);
    // Sent AFTER the unlock: the target may be spinning on this very
    // lock with interrupts disabled, and would not take the IPI.
    smp_send_reschedule(cpu_index);
}
```

- [ ] **Step 4: Run to verify it passes**

Run: `make test`
Expected: PASS, `[smp] steal selftest passed`, and the parallel selftest
from Task 13 still passes.

If the boot deadlocks here, the ordered pair is the first suspect:
confirm `steal_work` never calls `ready_pop(self)` while holding only
the victim's lock, and that `self != victim` always (the `c == self`
skip guarantees it).

- [ ] **Step 5: Commit**

```bash
git add kernel/sched/sched.c kernel/smp_selftest.c kernel/smp.h kernel/kernel.c
git commit -m "Phase 10: work stealing between per-CPU run queues"
```

---

### Task 16: TLB shootdown

The correctness-critical task. Without it, `mmaptest` and COW fork
corrupt memory once threads of one process run on two CPUs.

**Files:**
- Create: `kernel/tlb.c`, `kernel/tlb.h`
- Modify: `kernel/mm/paging.c`, `kernel/isr.c`, `kernel/smp.h`,
  `kernel/smp_selftest.c`

**Interfaces:**
- Produces:
  - `#define VECTOR_IPI_TLB 0xF1`
  - `void tlb_shootdown(uint64_t pml4_phys, uint64_t virt_start, uint64_t virt_end);`
  - `void tlb_defer_free(uint64_t phys, unsigned order);`
  - `void tlb_flush_deferred(void);`
  - `void ipi_tlb_handler(void);`

- [ ] **Step 1: Write the failing test**

Add to `kernel/smp_selftest.c`:

```c
void tlb_shootdown_selftest(void) {
    // The deferred-free queue is the correctness rule made testable: a
    // frame must NOT be back in pmm's free list until the shootdown
    // acknowledges. Freeing early is how a stale TLB entry ends up
    // pointing at another process's page.
    uint64_t frame = pmm_alloc(0);
    if (!frame) {
        serial_write_string("[tlb] selftest FAILED: no memory\n");
        return;
    }
    uint64_t free_before = pmm_free_frame_count();
    tlb_defer_free(frame, 0);
    if (pmm_free_frame_count() != free_before) {
        serial_write_string("[tlb] selftest FAILED: deferred free returned the frame early\n");
        return;
    }
    tlb_flush_deferred();
    if (pmm_free_frame_count() != free_before + 1) {
        serial_write_string("[tlb] selftest FAILED: deferred free never returned the frame\n");
        return;
    }

    // And the IPI itself must reach the other CPUs.
    uint64_t before = __atomic_load_n(&ipi_tlb_count, __ATOMIC_ACQUIRE);
    tlb_shootdown(0 /* all address spaces */, 0, 0);
    for (int i = 0; i < 1000000; i++) {
        if (__atomic_load_n(&ipi_tlb_count, __ATOMIC_ACQUIRE) > before) {
            serial_write_string("[tlb] shootdown selftest passed\n");
            return;
        }
        __asm__ volatile ("pause");
    }
    serial_write_string("[tlb] selftest FAILED: shootdown IPI never delivered\n");
}
```

- [ ] **Step 2: Run to verify it fails**

Run: `make test`
Expected: FAIL — build error, `'tlb_defer_free' undeclared`.

- [ ] **Step 3: Write the header**

Create `kernel/tlb.h`:

```c
#ifndef NEOOS_TLB_H
#define NEOOS_TLB_H

#include <stdint.h>

#define VECTOR_IPI_TLB 0xF1

extern volatile uint64_t ipi_tlb_count;

// Invalidates [virt_start, virt_end) on every CPU currently running a
// thread in the address space `pml4_phys`. Pass pml4_phys == 0 to
// target every CPU.
//
// MUST NOT be called holding mm_lock. See tlb.c for why.
void tlb_shootdown(uint64_t pml4_phys, uint64_t virt_start, uint64_t virt_end);

// Queues a frame to be returned to pmm only AFTER the next shootdown
// completes. Freeing a frame while another CPU may still hold a stale
// TLB entry for it is how one process ends up writing into another's
// memory.
void tlb_defer_free(uint64_t phys, unsigned order);

// Returns every queued frame to pmm. Called by tlb_shootdown once all
// acknowledgements are in.
void tlb_flush_deferred(void);

void ipi_tlb_handler(void);
void tlb_shootdown_selftest(void);

#endif
```

- [ ] **Step 4: Implement**

Create `kernel/tlb.c`:

```c
// kernel/tlb.c -- TLB shootdown.
//
// THE DEADLOCK THIS DESIGN AVOIDS: spin_lock_irqsave clears IF, so a
// CPU spinning on a lock cannot take an IPI. If the sender held mm_lock
// while waiting for acknowledgements, and the target were spinning on
// that same mm_lock, neither would ever move.
//
// So the sequence is fixed:
//   1. modify the page table UNDER mm_lock
//   2. record the range
//   3. RELEASE mm_lock
//   4. send the IPI and wait for acks WITH IF ENABLED
//
// and the rule that falls out of it: a frame goes back to pmm only
// after every ack is in.

#include "tlb.h"
#include "smp.h"
#include "lapic.h"
#include "lock.h"
#include "cpu_local.h"
#include "mm/pmm.h"
#include "serial.h"

volatile uint64_t ipi_tlb_count;

struct shootdown {
    volatile uint64_t pml4_phys;
    volatile uint64_t start;
    volatile uint64_t end;
    volatile int      pending;   // acks still outstanding
};

static struct shootdown active;
static struct spinlock  shootdown_lock;   // serializes shootdowns

#define DEFER_MAX 64
struct deferred { uint64_t phys; unsigned order; };
static struct deferred deferred[DEFER_MAX];
static int             deferred_n;
static struct spinlock deferred_lock;

void tlb_init(void) {
    spin_init(&shootdown_lock, LOCK_RANK_PROCESS, "tlb-shootdown");
    spin_init(&deferred_lock,  LOCK_RANK_PROCESS, "tlb-deferred");
}

void tlb_defer_free(uint64_t phys, unsigned order) {
    uint64_t f = spin_lock_irqsave(&deferred_lock);
    if (deferred_n < DEFER_MAX) {
        deferred[deferred_n].phys  = phys;
        deferred[deferred_n].order = order;
        deferred_n++;
        spin_unlock_irqrestore(&deferred_lock, f);
        return;
    }
    spin_unlock_irqrestore(&deferred_lock, f);
    // Queue full: shoot down now rather than drop the frame, then
    // retry. Correctness before throughput.
    tlb_shootdown(0, 0, 0);
    f = spin_lock_irqsave(&deferred_lock);
    deferred[deferred_n].phys  = phys;
    deferred[deferred_n].order = order;
    deferred_n++;
    spin_unlock_irqrestore(&deferred_lock, f);
}

void tlb_flush_deferred(void) {
    struct deferred local[DEFER_MAX];
    int n;
    uint64_t f = spin_lock_irqsave(&deferred_lock);
    n = deferred_n;
    for (int i = 0; i < n; i++) { local[i] = deferred[i]; }
    deferred_n = 0;
    spin_unlock_irqrestore(&deferred_lock, f);

    // pmm_free is called with NO lock held: it takes pmm_lock itself.
    for (int i = 0; i < n; i++) { pmm_free(local[i].phys, local[i].order); }
}

void ipi_tlb_handler(void) {
    // Invalidate locally, then acknowledge. A whole-CR3 reload is used
    // rather than per-page invlpg: ranges here are small but the
    // simplicity is worth more than the precision at this stage.
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");

    __atomic_fetch_add(&ipi_tlb_count, 1, __ATOMIC_ACQ_REL);
    __atomic_fetch_sub((int *)&active.pending, 1, __ATOMIC_ACQ_REL);
    lapic_send_eoi();
}

void tlb_shootdown(uint64_t pml4_phys, uint64_t virt_start, uint64_t virt_end) {
    if (lock_held_depth() != 0) {
        lock_panic("tlb_shootdown with a lock held", "tlb", 0);
    }

    uint64_t f = spin_lock_irqsave(&shootdown_lock);
    active.pml4_phys = pml4_phys;
    active.start     = virt_start;
    active.end       = virt_end;

    int self = (int)(this_cpu() - &cpus[0]);
    int online = smp_online_count();

    // Target only CPUs running a thread in this address space; a
    // shootdown for a single-threaded process usually sends nothing.
    int targets[MAX_CPUS];
    int ntargets = 0;
    for (int i = 0; i < online; i++) {
        if (i == self) { continue; }
        struct thread *cur = cpus[i].current;
        if (pml4_phys == 0 ||
            (cur && cur->proc && cur->proc->pml4_phys == pml4_phys)) {
            targets[ntargets++] = i;
        }
    }
    __atomic_store_n((int *)&active.pending, ntargets, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&shootdown_lock, f);

    // Local invalidation first: this CPU needs no IPI.
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");

    for (int i = 0; i < ntargets; i++) {
        lapic_send_ipi(smp_lapic_for_index(targets[i]), VECTOR_IPI_TLB);
    }

    // Wait for acks WITH INTERRUPTS ENABLED and NO LOCK HELD. Both
    // matter: a target may need to take an interrupt to make progress,
    // and holding a lock here is the deadlock described at the top.
    __asm__ volatile ("sti");
    int spins = 0;
    while (__atomic_load_n((int *)&active.pending, __ATOMIC_ACQUIRE) > 0) {
        __asm__ volatile ("pause");
        if (++spins > 100000000) {
            serial_write_string("[tlb] shootdown timed out; continuing\n");
            break;
        }
    }

    tlb_flush_deferred();
}
```

Register the handler in `kernel/isr.c`:

```c
        case VECTOR_IPI_TLB: ipi_tlb_handler(); break;
```

Call `tlb_init()` from `kmain` right after `heap_init()`, and
`tlb_shootdown_selftest()` after `smp_online_selftest()`.

- [ ] **Step 5: Defer the frees that matter**

In `kernel/mm/paging.c`, `paging_unmap_from(pml4, virt, free_frame)`
currently calls `pmm_free` directly. Change it to record the frame:

```c
        if (free_frame) {
            // NOT pmm_free: the frame must not be reusable until every
            // CPU that might hold a stale TLB entry for it has
            // acknowledged the shootdown.
            tlb_defer_free(frame_phys, 0);
        }
```

and have each caller — `munmap`, process teardown, and the COW fault
path — call `tlb_shootdown(p->pml4_phys, start, end)` **after
releasing `mm_lock`**.

- [ ] **Step 6: Run to verify it passes**

Run: `make test`
Expected: PASS, `[tlb] shootdown selftest passed`, and the `mmaptest`
userland suite still passes.

- [ ] **Step 7: Commit**

```bash
git add kernel/tlb.c kernel/tlb.h kernel/mm/paging.c kernel/isr.c kernel/smp_selftest.c kernel/smp.h kernel/kernel.c
git commit -m "Phase 10: TLB shootdown IPI and deferred frame release"
```

---

### Task 17: Panic-stop NMI

**Files:**
- Modify: `kernel/smp.c`, `kernel/smp.h`, `kernel/isr.c`, `kernel/lock.c`

**Interfaces:**
- Consumes: `lapic_send_nmi` (Task 12).
- Produces: `void smp_panic_stop_others(void);`, `void nmi_handler(void);`

- [ ] **Step 1: Write the failing test**

This one cannot be tested by triggering it — a real panic stops the
boot. Test the mechanism's preconditions instead. Add to
`kernel/smp_selftest.c`:

```c
void panic_stop_selftest(void) {
    // NMI, not a maskable vector: the entire point is stopping CPUs
    // that are spinning with interrupts disabled, which a normal
    // vector cannot do. Assert the delivery mode is what we think.
    if (VECTOR_IPI_PANIC != 0xF2) {
        serial_write_string("[smp] panic-stop selftest FAILED: vector moved\n");
        return;
    }
    // The handler must be reachable and must not take the serial lock.
    // A panicking CPU may be holding it, so a handler that waited on it
    // would hang the very stop it was sent to perform.
    if (smp_panic_handler_takes_serial_lock()) {
        serial_write_string("[smp] panic-stop selftest FAILED: handler touches the serial lock\n");
        return;
    }
    serial_write_string("[smp] panic-stop selftest passed\n");
}
```

with, in `kernel/smp.c`, a deliberately trivial:

```c
// Exists only so the selftest can assert the invariant it documents.
// Keep returning 0 -- and keep nmi_handler free of serial output.
int smp_panic_handler_takes_serial_lock(void) { return 0; }
```

- [ ] **Step 2: Run to verify it fails**

Run: `make test`
Expected: FAIL — build error, `'VECTOR_IPI_PANIC' undeclared`.

- [ ] **Step 3: Implement**

In `kernel/smp.h`:

```c
#define VECTOR_IPI_PANIC 0xF2

void smp_panic_stop_others(void);
void nmi_handler(void);
int  smp_panic_handler_takes_serial_lock(void);
```

In `kernel/smp.c`:

```c
// Broadcast as an NMI, not a maskable vector: a CPU spinning on a lock
// has IF clear and would never take a normal IPI. Stopping it is the
// whole purpose.
void smp_panic_stop_others(void) {
    int self = (int)(this_cpu() - &cpus[0]);
    int online = smp_online_count();
    for (int i = 0; i < online; i++) {
        if (i == self) { continue; }
        lapic_send_nmi(smp_lapic_for_index(i));
    }
}

// Deliberately silent. The panicking CPU may hold the serial lock, so
// printing here would hang the stop. Just freeze.
void nmi_handler(void) {
    for (;;) { __asm__ volatile ("cli; hlt"); }
}
```

Route vector 2 (NMI) in `kernel/isr.c` to `nmi_handler()`.

In `kernel/lock.c`, make `lock_panic` stop the others **before** it
prints, so no CPU scribbles over the evidence:

```c
void lock_panic(const char *msg, const char *a, const char *b) {
    smp_panic_stop_others();
    /* ... existing printing and halt, unchanged ... */
}
```

Do the same in the page-fault and double-fault handlers.

Call `panic_stop_selftest()` from `kmain` after `smp_online_selftest()`.

- [ ] **Step 4: Run to verify it passes**

Run: `make test`
Expected: PASS, `[smp] panic-stop selftest passed`, and the `faulter`
userland test still produces its expected fault output — confirming the
fault path still prints after the change.

- [ ] **Step 5: Commit**

```bash
git add kernel/smp.c kernel/smp.h kernel/isr.c kernel/lock.c kernel/smp_selftest.c kernel/kernel.c
git commit -m "Phase 10: panic-stop NMI freezes other CPUs before printing"
```

---

### Task 18: Userland surface

CLAUDE.md forbids leaving a user-facing kernel feature exposed only as a
raw syscall number.

**Files:**
- Modify: `kernel/syscall.c`, `kernel/syscall.h`, the musl shim in musl's
  arch directory, `docs/stdlib.md`
- Create: a userland check in `userland/` following the existing suites'
  pattern

**Interfaces:**
- Produces:
  - `SYS_CPU_COUNT`, `SYS_GETCPU` syscall numbers
  - `sysconf(_SC_NPROCESSORS_ONLN)`, `sysconf(_SC_NPROCESSORS_CONF)`
  - `sched_getcpu()`

- [ ] **Step 1: Write the failing test**

Create `userland/smptest.c` following the pattern of the existing
suites (read one, e.g. `userland/mmaptest.c`, and match its output
conventions exactly):

```c
#include <unistd.h>
#include <sched.h>

int main(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 2) {
        print("[smptest] FAILED: sysconf reports fewer than 2 cpus\n");
        return 1;
    }
    int cpu = sched_getcpu();
    if (cpu < 0 || cpu >= n) {
        print("[smptest] FAILED: sched_getcpu out of range\n");
        return 1;
    }
    print("[smptest] passed\n");
    return 0;
}
```

Add it to the Makefile's userland build and to `kmain`'s spawn list.

- [ ] **Step 2: Run to verify it fails**

Run: `make test`
Expected: FAIL — `[smptest] FAILED: sysconf reports fewer than 2 cpus`
(the syscalls return `-ENOSYS`).

- [ ] **Step 3: Add the syscalls**

In `kernel/syscall.h`, add two numbers after the highest existing one
(read the file — do not guess the next value). In `kernel/syscall.c`:

```c
    case SYS_CPU_COUNT:
        return smp_online_count();
    case SYS_GETCPU:
        return (int)(this_cpu() - &cpus[0]);
```

- [ ] **Step 4: Wire the musl shim**

Map musl's Linux syscall numbers onto the NeoOS numbers in the shim in
musl's arch directory, following the pattern the existing entries use.
`sched_getcpu` on Linux is `getcpu(2)`; forward it, and have `sysconf`'s
`_SC_NPROCESSORS_ONLN` / `_SC_NPROCESSORS_CONF` reach `SYS_CPU_COUNT`.

**Translation only, never emulation** (CLAUDE.md): if the shim starts
computing rather than forwarding, add the primitive to the kernel
instead.

- [ ] **Step 5: Document**

Add to `docs/stdlib.md` an entry for both, noting any divergence from
Linux. In particular: Linux's `getcpu` also returns a NUMA node; NeoOS
has no NUMA, so it always reports node 0. **Record that divergence
explicitly** — CLAUDE.md treats an unrecorded divergence as a bug.

- [ ] **Step 6: Run to verify it passes**

Run: `make test`
Expected: PASS, `[smptest] passed`.

- [ ] **Step 7: Commit**

```bash
git add kernel/syscall.c kernel/syscall.h userland/smptest.c docs/stdlib.md Makefile kernel/kernel.c
git commit -m "Phase 10: expose CPU count and current CPU through sysconf and sched_getcpu"
```

---

### Task 19: ABI compatibility report and roadmap update

Closing deliverable, required by CLAUDE.md's Linux ABI convention.

**Files:**
- Create: `docs/abi-compatibility.md`
- Modify: `OPTIMIZATION_SUMMARY.md`

- [ ] **Step 1: Inventory the actual boundary**

Do not write this from memory. Gather the facts first:

```bash
# Every syscall the kernel implements, with its number
grep -n 'case SYS_' kernel/syscall.c
grep -n '#define SYS_' kernel/syscall.h
# Every struct that crosses into userland
grep -rn 'struct stat\|struct dirent\|struct timespec\|struct sigaction' kernel/ lib/
# What the shim currently maps
ls musl/arch/*/  # locate the NeoOS shim and read it
```

- [ ] **Step 2: Write the report**

Create `docs/abi-compatibility.md` with these sections, filled from
Step 1's output, not from assumption:

1. **Scope** — a one-paragraph restatement of the convention: internals
   are ours, the userland-observable boundary must be Linux-shaped.
2. **Syscalls** — a table of every NeoOS syscall: number, name, the
   Linux syscall it corresponds to, and status
   (implemented / partial / stub / absent).
3. **Struct layouts** — for each struct crossing the boundary, whether
   field order, sizes and padding match Linux x86_64. Note any that
   were never checked, as unverified rather than matching.
4. **Constants and flags** — `O_*`, `PROT_*`, `MAP_*`, `SIG*`, `AT_*`,
   `CLOCK_*`, errno values: match or diverge.
5. **Process startup contract** — ELF entry, auxv, TLS setup, signal
   frame layout.
6. **Deliberate divergences** — each with its reason. Include the
   `getcpu` NUMA-node divergence from Task 18.
7. **What a ported application would still hit** — the honest list of
   gaps, ordered by how early a real program would trip on them.

- [ ] **Step 3: Update the roadmap**

In `OPTIMIZATION_SUMMARY.md`:
- Add the Phase 10 entry, following the Phase 8 entry's shape.
- **Correct the "Remaining Phases" section**: mark Phase 9 as closed
  (already satisfied by the existing `fs/ mm/ sched/ sync/` split) and
  Phase 10 as complete.
- **Correct the lock hierarchy block** to the renumbered ranks from
  Task 3, and remove the claim that `LOCK_RANK_RUNQUEUE` is unused.
- **Correct the "Carried forward as known work" list**: the per-CPU
  queues are no longer layout-only. Leave the `proc_list`/`wait4` and
  write-through-cache items, which this phase does not address.
- Update the performance table's "Scheduler lock contention" row, which
  currently reads "Layout only; no second CPU yet".

- [ ] **Step 4: Verify the whole suite one last time**

Run: `make test`
Expected: PASS. Confirm the log contains **all** of:
`[smp] topology selftest passed`, `[lock] selftest passed`,
`[pmm] selftest passed`, `[heap] selftest passed`,
`[runq] selftest passed`, `[waitq] lock selftest passed`,
`[vfs] selftest passed`, `[vma] selftest passed`,
`[cpu] local selftest passed`, `[smp] online selftest passed`,
`[smp] parallel selftest passed`,
`[smp] reschedule ipi selftest passed`,
`[smp] steal selftest passed`, `[tlb] shootdown selftest passed`,
`[smp] panic-stop selftest passed`, `[smptest] passed`,
plus the five pre-existing userland suites.

Also run `make test SMP_CPUS=1` and confirm the kernel still boots
cleanly on one CPU (the SMP-specific selftests will report failures
there by design — that is the documented pre-change red state, not a
regression).

- [ ] **Step 5: Commit**

```bash
git add docs/abi-compatibility.md OPTIMIZATION_SUMMARY.md
git commit -m "Phase 10: ABI compatibility report and roadmap update"
```

---

## Notes for the executor

- **The rank checker is your friend.** A rank-inversion panic names both
  locks and the CPU. Read it before guessing.
- **A hang with no output is almost always a lock held across
  `schedule()` or an IPI sent with interrupts disabled.** The Task 3
  assertion catches the first; the second is a design rule, not an
  assertion, so re-read §6.2 of the spec if a shootdown hangs.
- **Never index anything by `lapic_id`.** Use `smp_index_for_lapic()`.
- **`make test SMP_CPUS=1`** is the fastest way to tell an SMP bug from
  a general one.
