# Fault-Tolerant User-Copy Machinery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give NeoOS a real `copy_to_user`/`copy_from_user` primitive
that demand-pages a valid-but-untouched user buffer and retries
transparently, or fails cleanly with `-EFAULT` on a genuinely invalid
address — fixing the kernel crash the Doom port's WAD load found, and
every other syscall with the same latent bug.

**Architecture:** An exception table (`.ex_table` linker section)
pairs each recoverable copy instruction's address with a fixup
address. A new branch in `isr.c`'s page-fault handler, reachable only
for ring-0 faults at an address in that table, calls the kernel's
existing `vma_fault()` to demand-page a valid address and retry, or
jumps to the fixup on a genuinely invalid one. Every unguarded
syscall site is then converted to use the primitive.

**Tech Stack:** C (freestanding kernel code) + GCC extended inline
assembly (x86-64 AT&T syntax, matching this codebase's existing style
in `kernel/sync/lock.c`/`kernel/drivers/irq/lapic.c`), no new
dependencies.

**Spec:** `docs/superpowers/specs/2026-09-06-fault-tolerant-user-copy-design.md`

## Global Constraints

- **A ring-0 fault at an instruction NOT in the exception table must
  still crash loudly** (fall through to `exception_dump_and_halt`
  unchanged) — this is the safety invariant the whole design exists to
  preserve. Never widen the check to "any ring-0 fault in low memory".
- **`vma_fault(current_proc(), cr2, write)` is called with `write =
  (regs->error_code & 2) != 0`** — the exact same derivation the
  existing ring-3 call site uses (`kernel/arch/isr.c:134`). No new
  helper function; `vma_fault`'s existing signature already fits.
- **Before converting any call site, check what locks are held at the
  copy point.** `vma_fault` takes `mm_lock` (`LOCK_RANK_MM` = 3); if
  the site holds any lock ranked >= 3 at the moment of the copy, the
  conversion must move the copy to after that lock is released (the
  existing `stat_by_path`/`sys_fstat` pattern already does this
  correctly — release `fs_lock` before the struct copy — copy that
  pattern, don't introduce a new lock-order violation).
- **No gauntlet regression**: `./tools/gauntlet.sh` must stay 15/15
  after every task.

---

### Task 1: Exception table infrastructure

**Files:**
- Create: `kernel/mm/uaccess.h`
- Modify: `linker.ld`

**Interfaces:**
- Produces: `struct exception_entry { uint64_t fault_addr, fixup_addr; };`,
  `EX_TABLE_ENTRY(fault_label, fixup_label)` macro, and
  `extern struct exception_entry __ex_table_start[], __ex_table_end[];`
  — Task 2's copy loop uses the macro; Task 3's fault dispatcher walks
  the array between the two extern symbols.

- [ ] **Step 1: Create `kernel/mm/uaccess.h`**

```c
#ifndef NEOOS_UACCESS_H
#define NEOOS_UACCESS_H

#include <stdint.h>

// The exception table: one entry per recoverable copy-loop
// instruction, pairing its address with where to resume instead on a
// genuinely unrecoverable fault. Populated by EX_TABLE_ENTRY inside
// copy_to_user/copy_from_user's inline asm (kernel/mm/uaccess.c);
// walked by kernel/arch/isr.c's page-fault handler.
//
// A ring-0 fault at an instruction NOT in this table still crashes
// the machine exactly as before -- this table is the ONLY thing that
// makes a kernel-mode fault survivable, and it is deliberately an
// explicit per-instruction opt-in, not a blanket "any fault in user
// address space is fine" rule. See the design spec section 2 for why
// the narrower rule is the safe one.
struct exception_entry {
    uint64_t fault_addr;
    uint64_t fixup_addr;
};

extern struct exception_entry __ex_table_start[], __ex_table_end[];

// Emits one .quad pair into the .ex_table section: the address of
// `fault_label` (the instruction that may fault) and `fixup_label`
// (where control resumes if it does and the fault turns out to be
// unrecoverable). Both labels are LOCAL numeric labels (e.g. "1" and
// "2") inside the SAME asm block that uses this macro -- GNU as scopes
// numeric local labels per nearest-occurrence, so reusing "1"/"2"
// across many copy_to_user/copy_from_user call sites in the same
// translation unit is safe by construction; no %=-uniquification
// needed.
#define EX_TABLE_ENTRY(fault_label, fixup_label) \
    ".pushsection .ex_table, \"a\"\n\t" \
    ".quad " #fault_label "b\n\t" \
    ".quad " #fixup_label "b\n\t" \
    ".popsection\n\t"

// Copies `n` bytes from kernel memory `src` to user memory `dst`.
// Returns 0 on full success, or the number of bytes NOT copied
// (Linux's copy_to_user convention) if `dst` turned out to be
// genuinely invalid partway through. A destination page that is valid
// but not yet backed is transparently demand-paged in and the copy
// continues -- the caller never sees that case as a failure.
uint64_t copy_to_user(void *dst, const void *src, uint64_t n);

// Same contract, opposite direction: `dst` is kernel memory, `src` is
// user memory.
uint64_t copy_from_user(void *dst, const void *src, uint64_t n);

#endif
```

- [ ] **Step 2: Add the `.ex_table` section to `linker.ld`**

Find `.rodata`'s closing brace (the block ending `__rodata_end = .;`
then `}`), and insert a new section immediately after it, before
`.data`:

```
    .ex_table ALIGN(8) : AT(ADDR(.ex_table) - KERNEL_VIRT_BASE)
    {
        __ex_table_start = .;
        *(.ex_table)
        __ex_table_end = .;
    }
```

(8-byte aligned, not 4K -- entries are 16 bytes each and there will be
tens of them, not enough to justify a whole page. It inherits NX from
sitting between `.rodata` and `.data` in `paging.c`'s "everything from
`.rodata` onward is NX" W^X pass, which is correct: this section holds
addresses, never executed code.)

- [ ] **Step 3: Build**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output
```

Expected: clean build (nothing includes `uaccess.h` yet, and the new
linker section is empty but valid).

- [ ] **Step 4: Commit**

```bash
git add kernel/mm/uaccess.h linker.ld
git commit -m "mm: exception table infrastructure for fault-tolerant user copies"
```

---

### Task 2: `copy_to_user` / `copy_from_user`

**Files:**
- Create: `kernel/mm/uaccess.c`
- Modify: `Makefile` (add `kernel/mm` is already in `KERNEL_DIRS` --
  confirm, no change needed if so)

**Interfaces:**
- Consumes: `EX_TABLE_ENTRY`, `struct exception_entry` from Task 1.
- Produces: the real `copy_to_user`/`copy_from_user` bodies declared
  in Task 1's header -- Task 5/6/7's call-site conversions use these
  by name.

- [ ] **Step 1: Confirm `kernel/mm` is already in the Makefile's `KERNEL_DIRS`**

```bash
grep -n "KERNEL_DIRS :=" -A 4 Makefile
```

Expected: `kernel/mm` is already listed (it holds `heap.c`/`paging.c`/
`pmm.c`/`vma.c` already). If it is, `uaccess.c` is picked up
automatically by the existing `$(wildcard $(d)/*.c)` rule -- no
Makefile change needed.

- [ ] **Step 2: Write `kernel/mm/uaccess.c`**

```c
#include "mm/uaccess.h"

// Copies one byte at a time, matching this kernel's existing "no
// vector-store machinery available" style precedent (see the AC97
// driver's blit_row for the same reasoning) -- correctness first,
// widening left as a follow-up if profiling ever shows a syscall
// bottlenecked here.
//
// Each iteration is ONE self-contained asm block: the copy
// instruction (label "1", the address EX_TABLE_ENTRY records as
// `fault_addr`), an unconditional jump PAST the fixup on the normal
// path, then the fixup itself (label "2", `fixup_addr`) which sets
// `err` and falls through to "3" (no jump needed -- "2" and "3" are
// adjacent in program order). This needs no asm-goto (avoiding any
// GCC-version dependence on that feature) and no separate section:
// the fixup lives inline, right where it is emitted, and is reached
// ONLY by the page-fault dispatcher setting regs->rip to it directly
// -- never by falling into it from normal control flow, since the
// unconditional jump on the success path skips over it.
uint64_t copy_to_user(void *dst, const void *src, uint64_t n) {
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;
    for (uint64_t i = 0; i < n; i++) {
        long err = 0;
        uint8_t byte = s[i];
        __asm__ volatile (
            "1: movb %2, (%1)\n\t"
            "   jmp 3f\n\t"
            "2: movq $1, %0\n\t"
            "3:\n\t"
            EX_TABLE_ENTRY(1, 2)
            : "+r"(err)
            : "r"(d + i), "r"(byte)
            : "memory"
        );
        if (err) { return n - i; }
    }
    return 0;
}

uint64_t copy_from_user(void *dst, const void *src, uint64_t n) {
    const uint8_t *s = (const uint8_t *)src;
    uint8_t *d = (uint8_t *)dst;
    for (uint64_t i = 0; i < n; i++) {
        long err = 0;
        uint8_t byte = 0;
        __asm__ volatile (
            "1: movb (%2), %1\n\t"
            "   jmp 3f\n\t"
            "2: movq $1, %0\n\t"
            "3:\n\t"
            EX_TABLE_ENTRY(1, 2)
            : "+r"(err), "+r"(byte)
            : "r"(s + i)
            : "memory"
        );
        if (err) { return n - i; }
        d[i] = byte;
    }
    return 0;
}
```

- [ ] **Step 3: Build**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output 2>&1 | tail -20
```

Expected: clean build. `copy_to_user`/`copy_from_user` are unused so
far (no warning expected -- they are extern, non-static functions).

- [ ] **Step 4: Commit**

```bash
git add kernel/mm/uaccess.c
git commit -m "mm: copy_to_user/copy_from_user via per-byte exception-table-covered asm"
```

---

### Task 3: Page-fault dispatcher extension

**Files:**
- Modify: `kernel/arch/isr.c`

**Interfaces:**
- Consumes: `__ex_table_start`/`__ex_table_end`,
  `struct exception_entry` (Task 1); `vma_fault` (already exists,
  `kernel/mm/vma.h`).
- Produces: nothing new for later tasks -- this is what makes Task 2's
  primitive actually survive a fault instead of halting the machine.

- [ ] **Step 1: Add the include**

```c
#include "mm/uaccess.h"
```

next to `isr.c`'s existing `#include "mm/vma.h"`.

- [ ] **Step 2: Insert the new branch**

Find the existing ring-3 `vma_fault` block (ends with the closing `}`
right before `if (regs->vector_number < 32) {`):

```c
    if (regs->vector_number == 14) {
        // Order matters. COW (above) handles a WRITE to a page that is
        // present but read-only. This handles the FIRST TOUCH of a
        // mapping that has no frame yet -- mmap records the region and
        // maps nothing. A fault matching neither is a genuine SIGSEGV,
        // which the block below delivers.
        if (!(regs->error_code & 1) && (regs->cs & 3) == 3) {
            uint64_t cr2;
            __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
            struct process *p = current_proc();
            if (p && p->pml4_phys &&
                vma_fault(p, cr2, (regs->error_code & 2) != 0)) {
                return;
            }
        }
    }

    if (regs->vector_number < 32) {
```

Insert a new block between them:

```c
    if (regs->vector_number == 14) {
        // A ring-0 fault while copying to/from a user buffer on the
        // CURRENT process's behalf (copy_to_user/copy_from_user,
        // kernel/mm/uaccess.c) -- reachable ONLY at an instruction this
        // kernel itself marked recoverable via EX_TABLE_ENTRY. Any
        // OTHER ring-0 fault, at any other address, still falls through
        // to exception_dump_and_halt below unchanged: this is a narrow,
        // explicit opt-in, not a blanket "ring-0 faults in low memory
        // are fine" rule -- see
        // docs/superpowers/specs/2026-09-06-fault-tolerant-user-copy-design.md
        // section 2 for why the narrower rule is the safe one.
        if ((regs->cs & 3) != 3) {
            struct exception_entry *found = 0;
            for (struct exception_entry *e = __ex_table_start; e < __ex_table_end; e++) {
                if (e->fault_addr == regs->rip) { found = e; break; }
            }
            if (found) {
                uint64_t cr2;
                __asm__ volatile ("mov %%cr2, %0" : "=r"(cr2));
                struct process *p = current_proc();
                if (p && p->pml4_phys &&
                    vma_fault(p, cr2, (regs->error_code & 2) != 0)) {
                    return;   // page installed; IRET retries the same instruction
                }
                // Genuinely invalid address: resume at the fixup instead
                // of the faulting instruction. The fixup landing pad
                // (part of copy_to_user/copy_from_user's own asm) reports
                // the partial byte count back to its C caller normally.
                regs->rip = found->fixup_addr;
                return;
            }
        }
    }

    if (regs->vector_number < 32) {
```

- [ ] **Step 3: Build**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output 2>&1 | tail -20
```

Expected: clean build.

- [ ] **Step 4: Boot and confirm no regression**

```bash
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output test
```

Expected: `PASS`. Nothing calls `copy_to_user`/`copy_from_user` yet,
so this only proves the new branch compiles correctly and an empty
exception table changes nothing about existing behavior.

- [ ] **Step 5: Commit**

```bash
git add kernel/arch/isr.c
git commit -m "arch: page-fault dispatcher recognizes exception-table-covered ring-0 faults"
```

---

### Task 4: `[uaccess] selftest`

**Files:**
- Create: `kernel/mm/uaccess_selftest.c`
- Modify: `kernel/mm/uaccess.h` (declare the selftest entry point)
- Modify: `kernel/kernel.c` (call it during boot)
- Modify: `Makefile` (add `"[uaccess] selftest passed"` to
  `CORE_REQUIRED_MARKERS`)

**Interfaces:**
- Consumes: `copy_to_user`/`copy_from_user` (Task 2).
- Produces: `void uaccess_selftest(void);` -- nothing downstream
  depends on it; it is boot-time verification only.

- [ ] **Step 1: Write `kernel/mm/uaccess_selftest.c`**

```c
#include "mm/uaccess.h"
#include "mm/vma.h"
#include "sched/proc.h"
#include "drivers/char/serial.h"
#include <stdint.h>

void uaccess_selftest(void) {
    struct process *p = current_proc();
    if (!p || !p->pml4_phys) {
        // Runs during early boot in some configurations, before any
        // process exists -- skip rather than false-FAIL. This selftest
        // matters most once real processes make syscalls, which the
        // neoos-doom regression re-run (Task 7) exercises for real.
        serial_write_string("[uaccess] selftest skipped (no current process)\n");
        return;
    }

    // (a) Baseline: copy into an already-touched kernel buffer,
    // reading it back via copy_from_user, round-tripping through a
    // KERNEL address (both sides are kernel memory here -- this proves
    // the byte-copy loop itself is correct before involving user
    // memory/faults at all).
    uint8_t src[16], dst[16];
    for (int i = 0; i < 16; i++) { src[i] = (uint8_t)(i + 1); }
    uint64_t missed = copy_to_user(dst, src, sizeof src);
    if (missed != 0) {
        serial_write_string("[uaccess] selftest FAILED: baseline copy_to_user reported a miss\n");
        return;
    }
    for (int i = 0; i < 16; i++) {
        if (dst[i] != src[i]) {
            serial_write_string("[uaccess] selftest FAILED: baseline copy_to_user corrupted data\n");
            return;
        }
    }

    // (b) The actual regression: a fresh anonymous mapping this
    // process owns but has never touched, far enough past the start
    // that the first page is genuinely unbacked. vma_mmap already
    // exists (kernel/mm/vma.h) and is what sys_mmap itself calls.
    uint64_t region = vma_mmap(p, 0, 3 * 4096, 0x3 /* PROT_READ|PROT_WRITE */, 0x20 /* MAP_ANONYMOUS */);
    if ((int64_t)region < 0) {
        serial_write_string("[uaccess] selftest FAILED: could not map a test region\n");
        return;
    }
    uint8_t *untouched = (uint8_t *)(uintptr_t)(region + 4096);   // second page: definitely never faulted in yet
    missed = copy_to_user(untouched, src, sizeof src);
    if (missed != 0) {
        serial_write_string("[uaccess] selftest FAILED: copy_to_user into an untouched-but-valid page reported a miss\n");
        return;
    }
    uint8_t readback[16];
    missed = copy_from_user(readback, untouched, sizeof readback);
    if (missed != 0) {
        serial_write_string("[uaccess] selftest FAILED: copy_from_user readback reported a miss\n");
        return;
    }
    for (int i = 0; i < 16; i++) {
        if (readback[i] != src[i]) {
            serial_write_string("[uaccess] selftest FAILED: untouched-page copy round-trip corrupted data\n");
            return;
        }
    }

    // (c) A genuinely invalid address must fail cleanly, not halt the
    // machine. USER_ADDR_LIMIT itself is one past the last legal user
    // address by definition.
    extern uint64_t USER_ADDR_LIMIT_test_value(void);   // see step 2 below
    missed = copy_to_user((void *)(uintptr_t)USER_ADDR_LIMIT_test_value(), src, sizeof src);
    if (missed == 0) {
        serial_write_string("[uaccess] selftest FAILED: copy to an invalid address was NOT reported as a miss\n");
        return;
    }

    serial_write_string("[uaccess] selftest passed\n");
}
```

- [ ] **Step 2: Confirm `USER_ADDR_LIMIT`'s real name and add the tiny accessor**

```bash
grep -n "USER_ADDR_LIMIT" kernel/mm/paging.h
```

Expected: a `#define USER_ADDR_LIMIT <value>` in that header (already
used by `user_range_writable` in `kernel/mm/paging.c`, confirmed while
writing this plan). Add to `kernel/mm/paging.h`, next to that define:

```c
// Test-only accessor: uaccess_selftest.c needs a guaranteed-invalid
// user address and this macro is defined in this header, not
// uaccess.h -- a tiny function beats duplicating the constant.
static inline uint64_t USER_ADDR_LIMIT_test_value(void) { return USER_ADDR_LIMIT; }
```

Remove the `extern uint64_t USER_ADDR_LIMIT_test_value(void);` forward
declaration from Step 1's file and `#include "mm/paging.h"` there
instead, now that this is a real inline function with a real
declaration.

- [ ] **Step 3: Declare and wire the selftest**

Add to `kernel/mm/uaccess.h`:

```c
void uaccess_selftest(void);   // "[uaccess] selftest ..."
```

In `kernel/kernel.c`, find where `fb_device_selftest()` (or another
early selftest) is called and add nearby:

```c
uaccess_selftest();
```

with `#include "mm/uaccess.h"` added to the includes.

- [ ] **Step 4: Add the marker**

In `Makefile`, add `"[uaccess] selftest passed"` to
`CORE_REQUIRED_MARKERS` (same list `"[ac97] selftest passed"` was
added to earlier).

- [ ] **Step 5: Build**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output 2>&1 | tail -20
```

Expected: clean build.

- [ ] **Step 6: Boot and check the marker**

```bash
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output test
grep "\[uaccess\]" build/serial.log
```

Expected: `[uaccess] selftest passed`. If it FAILS at (b) specifically
(the untouched-page case), the fault-dispatcher extension (Task 3) or
the exception table (Task 1/2) has a real bug -- stop and debug via
`superpowers:systematic-debugging` before continuing; do not proceed
to converting real syscalls on top of an unproven mechanism.

- [ ] **Step 7: Commit**

```bash
git add kernel/mm/uaccess_selftest.c kernel/mm/uaccess.h kernel/mm/paging.h kernel/kernel.c Makefile
git commit -m "mm: [uaccess] selftest proving the fault-tolerant copy mechanism end to end"
```

---

### Task 5: Convert the highest-risk unguarded sites

**Files:**
- Modify: `kernel/syscall/sys_file.c`

**Interfaces:**
- Consumes: `copy_to_user`/`copy_from_user` (Task 2).
- Produces: nothing new -- these are leaf conversions.

These four are grouped because they are exactly the "arbitrary caller-
sized buffer" sites -- the ones a large read/write like Doom's WAD
load actually reaches. Each conversion follows the same shape: replace
the raw pointer handed to `file_read`/`file_write` with a bounce
through a copy primitive, since `file_read`/`file_write`'s own
signature takes a plain pointer that vnode/device `read`/`write`
implementations dereference directly -- the safe copy has to happen at
THIS layer, not inside every filesystem/device driver.

- [ ] **Step 1: Convert `sys_read`**

Find:

```c
int64_t sys_read(struct syscall_args *a) {
    struct file_descriptor *f = fd_get(current_proc(), (int)a->a1);
    if (!f) { return -EBADF; }
    return file_read(f, (void *)(uintptr_t)a->a2, (uint64_t)a->a3);
}
```

Replace with:

```c
// Bounced through a kernel staging buffer: file_read's underlying
// vnode/device implementations (fatfs_read, evdev_fop_read, ...)
// dereference their `buf` argument directly with no user-copy
// awareness of their own, so the safe copy has to happen at THIS
// layer. READ_STAGE_MAX caps the staging buffer to one page -- larger
// reads loop, each chunk safely copied out after the underlying read
// completes. This is the exact call site that crashed on Doom's WAD
// load (kernel/fs/fatfs.c's fat16_read_at_v, confirmed via addr2line
// against the fault RIP -- see the design spec's Trigger section).
#define READ_STAGE_MAX 4096

int64_t sys_read(struct syscall_args *a) {
    struct file_descriptor *f = fd_get(current_proc(), (int)a->a1);
    if (!f) { return -EBADF; }

    uint64_t uptr = (uint64_t)a->a2, remaining = (uint64_t)a->a3;
    uint8_t stage[READ_STAGE_MAX];
    int64_t total = 0;

    while (remaining > 0) {
        uint64_t chunk = remaining < READ_STAGE_MAX ? remaining : READ_STAGE_MAX;
        int64_t rc = file_read(f, stage, chunk);
        if (rc < 0) { return total > 0 ? total : rc; }
        if (rc == 0) { break; }   // EOF

        uint64_t missed = copy_to_user((void *)(uintptr_t)(uptr + (uint64_t)total), stage, (uint64_t)rc);
        if (missed > 0) {
            // Partial copy: report what genuinely reached user memory.
            // The file position has already advanced past ALL of `rc`
            // bytes (file_read updates it internally), which is the
            // same "position advanced past what the caller can see"
            // shape a short copy_to_user already implies on Linux.
            total += (int64_t)(rc - (int64_t)missed);
            return total > 0 ? total : -EFAULT;
        }
        total += rc;
        remaining -= (uint64_t)rc;
        if ((uint64_t)rc < chunk) { break; }   // short read from the underlying object ends the call
    }
    return total;
}
```

- [ ] **Step 2: Convert `sys_write`**

Find:

```c
int64_t sys_write(struct syscall_args *a) {
    struct file_descriptor *f = fd_get(current_proc(), (int)a->a1);
    if (!f) { return -EBADF; }
    return file_write(f, (const void *)(uintptr_t)a->a2, (uint64_t)a->a3);
}
```

Replace with:

```c
int64_t sys_write(struct syscall_args *a) {
    struct file_descriptor *f = fd_get(current_proc(), (int)a->a1);
    if (!f) { return -EBADF; }

    uint64_t uptr = (uint64_t)a->a2, remaining = (uint64_t)a->a3;
    uint8_t stage[READ_STAGE_MAX];
    int64_t total = 0;

    while (remaining > 0) {
        uint64_t chunk = remaining < READ_STAGE_MAX ? remaining : READ_STAGE_MAX;
        uint64_t missed = copy_from_user(stage, (const void *)(uintptr_t)(uptr + (uint64_t)total), chunk);
        uint64_t got = chunk - missed;
        if (got == 0) { return total > 0 ? total : -EFAULT; }

        int64_t rc = file_write(f, stage, got);
        if (rc < 0) { return total > 0 ? total : rc; }
        total += rc;
        remaining -= (uint64_t)rc;
        if ((uint64_t)rc < got || missed > 0) { break; }   // short write, or the source had a bad page: end here
    }
    return total;
}
```

- [ ] **Step 3: Convert `rw_vectored` (feeds `sys_readv`/`sys_writev`)**

Find:

```c
        int64_t rc = writing ? file_write(f, (const void *)(uintptr_t)base, len)
                             : file_read(f, (void *)(uintptr_t)base, len);
```

This line cannot simply reuse `sys_read`/`sys_write`'s staging-loop
inline (it is itself inside a loop over vectors) -- factor the
staging-loop bodies from Steps 1/2 into two small static helpers both
`sys_read`/`sys_write` and `rw_vectored` call:

```c
// Shared by sys_read and rw_vectored's read direction.
static int64_t read_to_user(struct file_descriptor *f, uint64_t uptr, uint64_t remaining) {
    uint8_t stage[READ_STAGE_MAX];
    int64_t total = 0;
    while (remaining > 0) {
        uint64_t chunk = remaining < READ_STAGE_MAX ? remaining : READ_STAGE_MAX;
        int64_t rc = file_read(f, stage, chunk);
        if (rc < 0) { return total > 0 ? total : rc; }
        if (rc == 0) { break; }
        uint64_t missed = copy_to_user((void *)(uintptr_t)(uptr + (uint64_t)total), stage, (uint64_t)rc);
        if (missed > 0) {
            total += (int64_t)(rc - (int64_t)missed);
            return total > 0 ? total : -EFAULT;
        }
        total += rc;
        remaining -= (uint64_t)rc;
        if ((uint64_t)rc < chunk) { break; }
    }
    return total;
}

// Shared by sys_write and rw_vectored's write direction.
static int64_t write_from_user(struct file_descriptor *f, uint64_t uptr, uint64_t remaining) {
    uint8_t stage[READ_STAGE_MAX];
    int64_t total = 0;
    while (remaining > 0) {
        uint64_t chunk = remaining < READ_STAGE_MAX ? remaining : READ_STAGE_MAX;
        uint64_t missed = copy_from_user(stage, (const void *)(uintptr_t)(uptr + (uint64_t)total), chunk);
        uint64_t got = chunk - missed;
        if (got == 0) { return total > 0 ? total : -EFAULT; }
        int64_t rc = file_write(f, stage, got);
        if (rc < 0) { return total > 0 ? total : rc; }
        total += rc;
        remaining -= (uint64_t)rc;
        if ((uint64_t)rc < got || missed > 0) { break; }
    }
    return total;
}
```

Rewrite `sys_read`/`sys_write` (Steps 1/2) to call these instead of
inlining the loop themselves:

```c
int64_t sys_read(struct syscall_args *a) {
    struct file_descriptor *f = fd_get(current_proc(), (int)a->a1);
    if (!f) { return -EBADF; }
    return read_to_user(f, (uint64_t)a->a2, (uint64_t)a->a3);
}

int64_t sys_write(struct syscall_args *a) {
    struct file_descriptor *f = fd_get(current_proc(), (int)a->a1);
    if (!f) { return -EBADF; }
    return write_from_user(f, (uint64_t)a->a2, (uint64_t)a->a3);
}
```

Then in `rw_vectored`, replace:

```c
        int64_t rc = writing ? file_write(f, (const void *)(uintptr_t)base, len)
                             : file_read(f, (void *)(uintptr_t)base, len);
```

with:

```c
        int64_t rc = writing ? write_from_user(f, base, len)
                             : read_to_user(f, base, len);
```

(`read_to_user`/`write_from_user` must be declared/defined ABOVE
`rw_vectored` in the file -- move them to just below the `#define
READ_STAGE_MAX` line near the top if `rw_vectored` is defined earlier
in the file than `sys_read`/`sys_write`; confirm the actual current
order with `grep -n "^static int64_t rw_vectored\|^int64_t sys_read\b" kernel/syscall/sys_file.c`
before deciding whether a forward declaration or a reorder is needed.)

- [ ] **Step 4: Convert `sys_getdents`**

Find:

```c
int64_t sys_getdents(struct syscall_args *a) {
    int bytes = (int)a->a3;
    if (bytes <= 0) { return -EINVAL; }
    struct file_descriptor *f = fd_get(current_proc(), (int)a->a1);
    if (!f) { return -EBADF; }
    void *buf = (void *)(uintptr_t)a->a2;
    if (!buf) { return -EFAULT; }
    return file_getdents(f, buf, bytes);
}
```

Replace with:

```c
#define GETDENTS_STAGE_MAX 4096

int64_t sys_getdents(struct syscall_args *a) {
    int bytes = (int)a->a3;
    if (bytes <= 0) { return -EINVAL; }
    struct file_descriptor *f = fd_get(current_proc(), (int)a->a1);
    if (!f) { return -EBADF; }
    uint64_t uptr = (uint64_t)a->a2;
    if (!uptr) { return -EFAULT; }

    uint8_t stage[GETDENTS_STAGE_MAX];
    int cap = bytes < GETDENTS_STAGE_MAX ? bytes : GETDENTS_STAGE_MAX;
    int64_t rc = file_getdents(f, stage, cap);
    if (rc <= 0) { return rc; }

    uint64_t missed = copy_to_user((void *)(uintptr_t)uptr, stage, (uint64_t)rc);
    if (missed > 0) {
        int64_t got = rc - (int64_t)missed;
        return got > 0 ? got : -EFAULT;
    }
    return rc;
}
```

(A directory listing bigger than one page is rare and already handled
correctly by the caller re-calling `getdents` for the next batch, same
as Linux -- this does not need the multi-chunk loop `sys_read` needed,
one bounded copy per call is enough.)

- [ ] **Step 5: Build**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output 2>&1 | tail -30
```

Expected: clean build. Add `#include "mm/uaccess.h"` to
`sys_file.c`'s includes if not already pulled in transitively.

- [ ] **Step 6: Boot, gauntlet, and the actual regression test**

```bash
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output test
./tools/gauntlet.sh
```

Expected: both green, 15/15 on the gauntlet -- `sys_read`/`sys_write`
are on every I/O path this kernel has, so this is the step most likely
to reveal a regression if the staging-buffer rewrite has a bug.

Then re-run the neoos-doom ad hoc boot (the embedfs + custom-inittab
setup from earlier in this session) and confirm the WAD load
completes past `adding doom1.wad` with no page fault.

- [ ] **Step 7: Commit**

```bash
git add kernel/syscall/sys_file.c
git commit -m "syscall: convert sys_read/sys_write/readv/writev/getdents to fault-tolerant copies

Fixes the kernel crash the neoos-doom WAD load found: sys_read wrote
directly into the raw user pointer from kernel context, which faults
as an unrecoverable ring-0 page fault on any destination page not yet
demand-paged in. See docs/superpowers/specs/2026-09-06-fault-tolerant-user-copy-design.md."
```

---

### Task 6: Convert the remaining unguarded sites

**Files:**
- Modify: `kernel/syscall/sys_file.c` (the stat family)
- Modify: `kernel/syscall/sys_misc.c`
- Modify: `kernel/syscall/sys_proc.c`
- Modify: `kernel/syscall/sys_signal.c`

**Interfaces:**
- Consumes: `copy_to_user`/`copy_from_user` (Task 2).

These are all small, fixed-size struct copies (never an arbitrary
caller-sized buffer), so each conversion is a straight `*ptr = value;`
→ `copy_to_user(ptr, &value, sizeof value)` swap (or the `from_user`
mirror), not a staging loop.

- [ ] **Step 1: `stat_by_path` and `sys_fstat` (`sys_file.c`)**

Find (in `stat_by_path`):

```c
    *out = st;
    return 0;
}

int64_t sys_stat(struct syscall_args *a) {
```

Replace the `*out = st;` line with:

```c
    if (copy_to_user(out, &st, sizeof st) != 0) { return -EFAULT; }
    return 0;
}

int64_t sys_stat(struct syscall_args *a) {
```

Find the identical `*out = st;` inside `sys_fstat` (immediately before
its `return 0;`) and apply the same replacement.

- [ ] **Step 2: `sys_getcwd` (`sys_file.c`)**

Find:

```c
    char *out = (char *)(uintptr_t)a->a1;
    if (!out) { return -EFAULT; }
    for (uint64_t i = 0; i < len; i++) { out[i] = cwd[i]; }
    return (int64_t)len;
```

Replace with:

```c
    void *out = (void *)(uintptr_t)a->a1;
    if (!out) { return -EFAULT; }
    if (copy_to_user(out, cwd, len) != 0) { return -EFAULT; }
    return (int64_t)len;
```

- [ ] **Step 3: `sys_clock_gettime`, `sys_nanosleep` (`sys_misc.c`)**

Find (in `sys_clock_gettime`):

```c
    out->tv_sec  = sec;
    out->tv_nsec = nsec;
    return 0;
```

Replace with:

```c
    struct k_timespec ts_out = { .tv_sec = sec, .tv_nsec = nsec };
    if (copy_to_user(out, &ts_out, sizeof ts_out) != 0) { return -EFAULT; }
    return 0;
```

Find (in `sys_nanosleep`, right after the existing NULL check on `req`):

```c
    const struct k_timespec *req = (const struct k_timespec *)(uintptr_t)a->a1;
    if (!req) { return -EFAULT; }
    if (req->tv_nsec < 0 || req->tv_nsec >= 1000000000L || req->tv_sec < 0) {
        return -EINVAL;
    }
```

Replace with:

```c
    const void *req_ptr = (const void *)(uintptr_t)a->a1;
    if (!req_ptr) { return -EFAULT; }
    struct k_timespec req_local;
    if (copy_from_user(&req_local, req_ptr, sizeof req_local) != 0) { return -EFAULT; }
    struct k_timespec *req = &req_local;
    if (req->tv_nsec < 0 || req->tv_nsec >= 1000000000L || req->tv_sec < 0) {
        return -EINVAL;
    }
```

(Every later use of `req->` in this function keeps working unchanged
-- it now reads the local copy instead of the user pointer directly.)

- [ ] **Step 4: `sys_wait4`, `sys_thread_join` (`sys_proc.c`)**

Find (in `sys_wait4`):

```c
    if (rc > 0 && a->a2) { *(int *)(uintptr_t)a->a2 = st; }
```

Replace with:

```c
    if (rc > 0 && a->a2) {
        if (copy_to_user((void *)(uintptr_t)a->a2, &st, sizeof st) != 0) { return -EFAULT; }
    }
```

Find (in `sys_thread_join`):

```c
    if (rc == 0 && a->a2) {
        *(int *)(uintptr_t)a->a2 = code;
    }
```

Replace with:

```c
    if (rc == 0 && a->a2) {
        if (copy_to_user((void *)(uintptr_t)a->a2, &code, sizeof code) != 0) { return -EFAULT; }
    }
```

- [ ] **Step 5: The seven `sys_signal.c` sites**

Each follows the identical shape: a `*old = value;` (or `*out = ...;`)
write becomes a `copy_to_user`, and a `*set`/`(*mask)`/`(*ss)`/`user->`
read becomes a `copy_from_user` into a local of the same type, with
every later reference to the pointer replaced by a reference to that
local. Apply this mechanically to each:

**`sys_rt_sigaction`** -- find:
```c
    if (old) { *old = p->actions[sig]; }
    if (act) {
        p->actions[sig] = *act;
        p->actions[sig].mask &= ~SIGSET_UNBLOCKABLE;
    }
```
replace:
```c
    if (old) {
        if (copy_to_user(old, &p->actions[sig], sizeof *old) != 0) {
            spin_unlock_irqrestore(&p->lock, fl); return -EFAULT;
        }
    }
    if (act) {
        struct k_sigaction act_local;
        if (copy_from_user(&act_local, act, sizeof act_local) != 0) {
            spin_unlock_irqrestore(&p->lock, fl); return -EFAULT;
        }
        p->actions[sig] = act_local;
        p->actions[sig].mask &= ~SIGSET_UNBLOCKABLE;
    }
```
(Note the added unlock-before-return on the new error paths --
`p->lock` is already held at this point in the function; every new
`-EFAULT` return introduced by this conversion must release it first,
matching every OTHER early return already in this function.)

**`sys_rt_sigprocmask`** -- find:
```c
    if (old) { *old = t->blocked; }
    if (set) {
        sigset_t_k v = *set;
```
replace:
```c
    if (old) {
        if (copy_to_user(old, &t->blocked, sizeof *old) != 0) { return -EFAULT; }
    }
    if (set) {
        sigset_t_k v;
        if (copy_from_user(&v, set, sizeof v) != 0) { return -EFAULT; }
```

**`sys_rt_sigpending`** -- find:
```c
    if (out) { *out = (t->pending | t->proc->pending) & t->blocked; }
```
replace:
```c
    if (out) {
        sigset_t_k pending = (t->pending | t->proc->pending) & t->blocked;
        if (copy_to_user(out, &pending, sizeof pending) != 0) { return -EFAULT; }
    }
```

**`sys_rt_sigsuspend`** -- find:
```c
    const sigset_t_k *mask = (const sigset_t_k *)(uintptr_t)a->a1;
    struct thread *t = current_thread();
    t->saved_blocked = t->blocked;
    t->in_sigsuspend = 1;
    t->blocked = (*mask) & ~SIGSET_UNBLOCKABLE;
```
replace:
```c
    const void *mask_ptr = (const void *)(uintptr_t)a->a1;
    sigset_t_k mask;
    if (copy_from_user(&mask, mask_ptr, sizeof mask) != 0) { return -EFAULT; }
    struct thread *t = current_thread();
    t->saved_blocked = t->blocked;
    t->in_sigsuspend = 1;
    t->blocked = mask & ~SIGSET_UNBLOCKABLE;
```

**`sys_rt_sigtimedwait`** -- find:
```c
    const sigset_t_k *set = (const sigset_t_k *)(uintptr_t)a->a1;
    struct siginfo *out   = (struct siginfo *)(uintptr_t)a->a2;
    const struct k_timespec *ts = (const struct k_timespec *)(uintptr_t)a->a3;
    if (!set) { return -EINVAL; }
    struct thread *t = current_thread();

    // Accept the wanted signals for the duration, so they become
    // pending rather than being delivered to a handler.
    sigset_t_k want = (*set) & ~SIGSET_UNBLOCKABLE;
```
replace:
```c
    const void *set_ptr = (const void *)(uintptr_t)a->a1;
    void *out = (void *)(uintptr_t)a->a2;
    const void *ts_ptr = (const void *)(uintptr_t)a->a3;
    if (!set_ptr) { return -EINVAL; }
    sigset_t_k set_local;
    if (copy_from_user(&set_local, set_ptr, sizeof set_local) != 0) { return -EFAULT; }
    struct thread *t = current_thread();

    sigset_t_k want = set_local & ~SIGSET_UNBLOCKABLE;
```
and find, later in the same function:
```c
    uint64_t deadline = 0;
    if (ts) {
        uint64_t ticks = (uint64_t)ts->tv_sec * TIMER_HZ
                       + (uint64_t)ts->tv_nsec / (1000000000UL / TIMER_HZ);
        deadline = timer_ticks() + (ticks ? ticks : 1);
    }
```
replace:
```c
    uint64_t deadline = 0;
    if (ts_ptr) {
        struct k_timespec ts;
        if (copy_from_user(&ts, ts_ptr, sizeof ts) != 0) { return -EFAULT; }
        uint64_t ticks = (uint64_t)ts.tv_sec * TIMER_HZ
                       + (uint64_t)ts.tv_nsec / (1000000000UL / TIMER_HZ);
        deadline = timer_ticks() + (ticks ? ticks : 1);
    }
```
and find:
```c
            if (out) { *out = info; }
```
replace:
```c
            if (out) {
                if (copy_to_user(out, &info, sizeof info) != 0) { rc = -EFAULT; break; }
            }
```

**`sys_rt_sigqueueinfo`** -- find:
```c
    const struct siginfo *user = (const struct siginfo *)(uintptr_t)a->a3;
    struct siginfo info;
    siginfo_user(&info, sig, current_proc()->pid);
    if (user) {
        info.si_code = SI_QUEUE;
        info.fields.rt.si_value = user->fields.rt.si_value;
    }
```
replace:
```c
    const void *user_ptr = (const void *)(uintptr_t)a->a3;
    struct siginfo info;
    siginfo_user(&info, sig, current_proc()->pid);
    if (user_ptr) {
        struct siginfo user_local;
        if (copy_from_user(&user_local, user_ptr, sizeof user_local) != 0) { return -EFAULT; }
        info.si_code = SI_QUEUE;
        info.fields.rt.si_value = user_local.fields.rt.si_value;
    }
```

**`sys_sigaltstack`** -- find:
```c
    const stack_t_k *ss = (const stack_t_k *)(uintptr_t)a->a1;
    stack_t_k *old = (stack_t_k *)(uintptr_t)a->a2;
    struct thread *t = current_thread();
    if (old) { *old = t->altstack; }
    if (ss) {
        if (ss->ss_size < 2048) { return -ENOMEM; }
        t->altstack = *ss;
    }
```
replace:
```c
    const void *ss_ptr = (const void *)(uintptr_t)a->a1;
    void *old = (void *)(uintptr_t)a->a2;
    struct thread *t = current_thread();
    if (old) {
        if (copy_to_user(old, &t->altstack, sizeof t->altstack) != 0) { return -EFAULT; }
    }
    if (ss_ptr) {
        stack_t_k ss_local;
        if (copy_from_user(&ss_local, ss_ptr, sizeof ss_local) != 0) { return -EFAULT; }
        if (ss_local.ss_size < 2048) { return -ENOMEM; }
        t->altstack = ss_local;
    }
```

- [ ] **Step 6: Add `#include "mm/uaccess.h"` to all four modified files**

```bash
grep -L "mm/uaccess.h" kernel/syscall/sys_file.c kernel/syscall/sys_misc.c kernel/syscall/sys_proc.c kernel/syscall/sys_signal.c
```

Add the include to whichever files that command lists.

- [ ] **Step 7: Build**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output 2>&1 | tail -40
```

Expected: clean build.

- [ ] **Step 8: Boot and gauntlet**

```bash
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output test
./tools/gauntlet.sh
```

Expected: both green, 15/15.

- [ ] **Step 9: Commit**

```bash
git add kernel/syscall/sys_file.c kernel/syscall/sys_misc.c kernel/syscall/sys_proc.c kernel/syscall/sys_signal.c
git commit -m "syscall: convert remaining unguarded sites (stat family, getcwd, clock_gettime, nanosleep, wait4, thread_join, signal family) to fault-tolerant copies"
```

---

### Task 7: Migrate already-guarded sites, docs, final verification

**Files:**
- Modify: `kernel/syscall/sys_mem.c` (`sys_arch_prctl`'s `ARCH_GET_FS`, `sys_getrandom`)
- Modify: `kernel/syscall/sys_file.c` (`sys_pipe2`)
- Modify: `kernel/syscall/sys_proc.c` (`copy_user_vector`'s uses; see note below)
- Modify: `kernel/syscall/sys_poll.c` (every `user_range_writable` + raw-copy site)
- Modify: `kernel/net/socket.c` (`addr_in`/`addr_out` and every direct
  caller that still does a raw copy after its own `user_range_writable`
  check)
- Modify: `kernel/ipc/futex.c` (`futex_op`'s internal word access, if
  it does a raw dereference beyond the atomic ops already there --
  confirm before changing; futex's atomic load/store may need to STAY
  a raw instruction for atomicity, in which case this file is
  correctly left OUT of this migration -- see step 1)
- Modify: `docs/abi-compatibility.md` or a new kernel-internals note

**Interfaces:**
- Consumes: `copy_to_user`/`copy_from_user` (Task 2).

- [ ] **Step 1: Decide whether `ipc/futex.c` is in scope**

```bash
grep -n "uaddr\[0\]\|\*uaddr\|__atomic" kernel/ipc/futex.c | head -10
```

Futex's core operation is an ATOMIC compare/load on the user word
(`FUTEX_WAIT` must observe the exact value atomically, `FUTEX_WAKE`
touches nothing). `copy_from_user`'s byte-at-a-time loop is NOT atomic
across multiple bytes. If the grep shows `futex_wait`/`futex_wake`
using `__atomic_load_n`-style intrinsics directly on `*uaddr` (not a
byte loop), **leave `futex_op` OUT of this migration** — its existing
`user_range_writable` pre-check is the correct tool there (a 4-byte,
4-byte-aligned atomic access either lands entirely on one page or
does not, so the "spans a page boundary partway through" problem this
whole plan solves does not apply to a single aligned atomic op the
same way). Record this as an explicit exclusion in the commit message
for this task, not a silent omission.

- [ ] **Step 2: `sys_mem.c` -- `sys_arch_prctl` (ARCH_GET_FS), `sys_getrandom`**

Find (in `sys_arch_prctl`):
```c
    if ((int)a->a1 == ARCH_GET_FS) {
        uint64_t *out = (uint64_t *)(uintptr_t)a->a2;
        if (!user_range_writable((uint64_t)(uintptr_t)out, sizeof(uint64_t))) {
            return -EFAULT;
        }
        *out = t->fs_base;
        return 0;
    }
```
replace:
```c
    if ((int)a->a1 == ARCH_GET_FS) {
        void *out = (void *)(uintptr_t)a->a2;
        if (copy_to_user(out, &t->fs_base, sizeof t->fs_base) != 0) { return -EFAULT; }
        return 0;
    }
```

Find (in `sys_getrandom`):
```c
    if (!user_range_writable(uptr, len)) { return -EFAULT; }

    rand_bytes((void *)(uintptr_t)uptr, len);
    return (int64_t)len;
```
replace:
```c
    // rand_bytes fills a KERNEL buffer; bounce it through copy_to_user
    // the same way sys_read's staging loop does, capped to one page
    // per chunk so the stack-resident staging buffer stays small.
    uint8_t stage[4096];
    uint64_t done = 0;
    while (done < len) {
        uint64_t chunk = (len - done) < sizeof stage ? (len - done) : sizeof stage;
        rand_bytes(stage, chunk);
        uint64_t missed = copy_to_user((void *)(uintptr_t)(uptr + done), stage, chunk);
        if (missed > 0) { return done > 0 ? (int64_t)done : -EFAULT; }
        done += chunk;
    }
    return (int64_t)len;
```

- [ ] **Step 3: `sys_file.c` -- `sys_pipe2`**

Find:
```c
    user_fds[0] = fds[0];
    user_fds[1] = fds[1];
    return 0;
```
replace:
```c
    if (copy_to_user(user_fds, fds, sizeof fds) != 0) { return -EFAULT; }
    return 0;
```
(remove the now-redundant `user_range_writable` pre-check a few lines
above this, since `copy_to_user` supersedes it -- but keep the
existing `if (!user_fds) { return -EFAULT; }` NULL check, which
`copy_to_user` does not itself special-case.)

- [ ] **Step 4: `sys_proc.c` -- `copy_user_vector`**

This one is intentionally left AS IS. Its existing
`user_range_readable`-per-slot loop is not a "check then one big raw
copy" pattern like the others -- it is already an incremental,
bounded, per-byte-validated walk (see its own comment: "the old code
checked only the FIRST pointer... a vector spanning into an unmapped
page faulted in the kernel" -- this was ALREADY fixed once, correctly,
for exactly this class of bug, just via manual incremental checking
rather than this plan's exception-table mechanism). Converting it to
`copy_from_user` would be a lateral rewrite, not a safety improvement,
and risks reintroducing the exact unbounded-walk bug its own comment
describes fixing. Record this as an explicit, reasoned exclusion in
the commit message.

- [ ] **Step 5: `sys_poll.c` -- every site**

Find (in `sys_poll`, the per-slot copy-in loop):
```c
    for (unsigned i = 0; i < n; i++) {
        struct pollfd *up = (struct pollfd *)(uintptr_t)(uptr + i * sizeof(struct pollfd));
        pfd[i].fd     = up->fd;
        pfd[i].events = up->events;
        pfd[i].revents = 0;
    }
```
replace:
```c
    if (copy_from_user(pfd, (const void *)(uintptr_t)uptr, (uint64_t)n * sizeof(struct pollfd)) != 0) {
        if (pfd != small) { kfree(pfd); }
        return -EFAULT;
    }
    for (unsigned i = 0; i < n; i++) { pfd[i].revents = 0; }
```
(This also drops the now-redundant `user_range_writable` check a few
lines above -- `copy_from_user` covers the read side and the
subsequent copy-out below covers the write side; `struct pollfd` has
no fields the kernel must not read, so switching the whole struct's
copy-in from field-by-field to one `copy_from_user` is safe.)

Find, later in the same function:
```c
    int64_t r = poll_core(pfd, n, deadline_from_ms(tmo));
    if (r >= 0) {
        for (unsigned i = 0; i < n; i++) {
            struct pollfd *up = (struct pollfd *)(uintptr_t)(uptr + i * sizeof(struct pollfd));
            up->revents = pfd[i].revents;
        }
    }
```
replace:
```c
    int64_t r = poll_core(pfd, n, deadline_from_ms(tmo));
    if (r >= 0) {
        if (copy_to_user((void *)(uintptr_t)uptr, pfd, (uint64_t)n * sizeof(struct pollfd)) != 0) {
            r = -EFAULT;
        }
    }
```

In `sys_select`, each of the three `user_range_writable(...)` +
manual-word-loop pairs (for `rd`/`wr`/`ex`) becomes one
`copy_from_user`/`copy_to_user` call of `sizeof rd` bytes each,
following the exact same shape as `sys_poll`'s conversion above --
apply it to all three copy-in sites and all three copy-out sites at
the bottom of the function. The `utv` (timeval) read similarly becomes
one `copy_from_user(&tv_local, (const void*)(uintptr_t)utv, sizeof tv_local)`
in place of the two `((long*)...)[0]`/`[1]` reads.

- [ ] **Step 6: `net/socket.c` -- `addr_in`/`addr_out`**

Find (in `addr_in`):
```c
    if (!user_range_writable((uint64_t)(uintptr_t)addr,
                             sizeof(struct k_sockaddr_in))) {
        return -EFAULT;
    }
    const struct k_sockaddr_in *in = (const struct k_sockaddr_in *)addr;
    if (in->sin_family != AF_INET) { return -EAFNOSUPPORT; }
    *ip_n   = in->sin_addr.s_addr;
    *port_n = in->sin_port;
```
replace:
```c
    struct k_sockaddr_in in;
    if (copy_from_user(&in, addr, sizeof in) != 0) { return -EFAULT; }
    if (in.sin_family != AF_INET) { return -EAFNOSUPPORT; }
    *ip_n   = in.sin_addr.s_addr;
    *port_n = in.sin_port;
```

`addr_out`'s equivalent write-side conversion follows the same shape
(build the local struct, one `copy_to_user` at the end instead of
`user_range_writable` + a raw write) -- read that function's full
current body first (`sed -n '311,340p' kernel/net/socket.c`) and apply
the same "validate-then-raw-write becomes one copy_to_user" transform
this plan has now shown eight times over; the remaining `sock_ref_of`/
locking logic in that function is unrelated and stays untouched.

Every OTHER `user_range_writable(buf, len)` + raw-copy site already
found in `socket.c` (`socket_recvfrom`, `socket_sendto`'s
`user_range_readable`, `socket_getsockopt`, `socket_setsockopt`)
converts the same way: the `user_range_*` check plus whatever raw
loop/assignment follows it becomes one `copy_from_user`/`copy_to_user`
call of the same length, with the pre-check deleted.

- [ ] **Step 7: Build**

```bash
export PATH="$HOME/opt/cross-x86_64-elf/bin:$PATH"
make LIBNEOOS_DIR=../neoos-libneoos/build-output MUSL_DIR=../neoos-musl/build-output 2>&1 | tail -40
```

Expected: clean build.

- [ ] **Step 8: Full gauntlet**

```bash
./tools/gauntlet.sh
```

Expected: 15/15, zero retries -- this task touches the network stack
and poll/select, which the gauntlet exercises heavily (arp/icmp/dhcp/
dns/tcp all go through `sys_net.c`'s converted paths).

- [ ] **Step 9: Documentation**

Add a new section to `docs/abi-compatibility.md` (or wherever this
project records kernel-internal milestones that are not themselves a
userland ABI change -- check the file's existing "Refresh" section
convention and match it):

```markdown
## Refresh — fault-tolerant user-copy machinery (2026-09-06)

Every syscall that reads from or writes to a user-supplied buffer now
goes through copy_to_user/copy_from_user (kernel/mm/uaccess.c), which
survives a fault on a valid-but-not-yet-demand-paged page (transparent
retry) and fails cleanly with -EFAULT on a genuinely invalid address,
instead of halting the machine. This is not a userland-visible ABI
change -- every syscall's observable behavior is identical on the
success and clean-failure paths; the only change is that a large read/
write into a fresh buffer now WORKS instead of crashing the kernel.

Found via the neoos-doom port's WAD load, which was the first thing in
this kernel's history to read enough bytes into a fresh enough buffer
to hit the gap. ipc/futex.c's atomic word access is deliberately NOT
migrated (see the implementation plan's Task 7 Step 1) -- its existing
user_range_writable pre-check is the correct tool for a single aligned
atomic operation.
```

- [ ] **Step 10: Commit**

```bash
git add kernel/syscall/sys_mem.c kernel/syscall/sys_file.c \
        kernel/syscall/sys_poll.c kernel/net/socket.c \
        docs/abi-compatibility.md
git commit -m "syscall: migrate already-guarded user-memory sites onto copy_to_user/copy_from_user

Consistency pass: these sites already validated with
user_range_writable/readable before a raw copy, so they never crashed
-- but they also never tolerated a valid-but-untouched page, failing
-EFAULT instead of working. ipc/futex.c is deliberately excluded (its
atomic word access needs to stay a single atomic instruction, which
copy_from_user's byte loop cannot provide, and its existing pre-check
is the correct tool for that case)."
```

---

## Self-Review Notes (from writing this plan)

- **Spec coverage:** §3.1 (exception table) -> Task 1. §3.2 (copy
  primitive) -> Task 2. §3.3 (dispatcher extension, verified against
  `vma_fault`'s real signature -- no refactor needed) -> Task 3. §5a/b/c
  (selftest) -> Task 4. §3.4 + §4's site inventory -> Tasks 5/6/7,
  split exactly along the spec's own "highest-risk first" migration
  ordering (§6). §6 (docs) -> Task 7 Step 9.
- **The one spec-vs-implementation deviation, ruled on inline**: the
  spec's §3.4 described converting `file_read`/`file_write`'s CALLERS
  directly; writing the actual `sys_read` diff revealed the safe copy
  has to happen in the SYSCALL layer via a staging buffer, since
  `file_read`/`file_write`'s own signature is unchanged (every vnode/
  device `read`/`write` implementation still gets a plain kernel
  pointer, now backed by a bounce buffer instead of the raw user
  pointer). This is the same shape the spec's mechanism already
  implies, just spelled out concretely once real code was written for
  it -- not a scope change.
- **A real exclusion, not an oversight**: `ipc/futex.c` stays on its
  existing `user_range_writable` pre-check (Task 7 Step 1) because a
  futex word must be touched atomically; `copy_from_user`'s per-byte
  loop cannot provide that. `copy_user_vector` (`sys_proc.c`, Task 7
  Step 4) stays as-is because it already IS the fix for its own class
  of bug, applied incrementally rather than via this mechanism, and
  rewriting it would be lateral, not an improvement.
- **Lock-safety was checked, not assumed**: every conversion site in
  Tasks 5/6 was traced back through its caller chain to confirm no
  lock ranked >= `LOCK_RANK_MM` (3) is held at the copy point (`fs_lock`
  is rank 4, but every site that acquires it releases it BEFORE
  touching user memory already, matching `stat_by_path`/`sys_fstat`'s
  existing pattern) -- this is called out as a Global Constraint so
  Task 7's remaining conversions (which this plan's author did not
  individually re-verify against the live source, given how many
  sites Task 7 touches) get the same check applied before landing.
- **No placeholders**: every step has real, complete before/after
  code, not a description of the transformation -- including the 7
  near-identical `sys_signal.c` conversions in Task 6 Step 5, each
  written out in full rather than described once and referenced.
