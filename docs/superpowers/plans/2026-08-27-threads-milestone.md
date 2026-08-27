# Threads Milestone Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split `struct task` into `struct process` and `struct thread`, add spinlocks, wait queues and per-CPU data, and expose `thread_create`/`thread_join`/`thread_exit`/`thread_self` to user mode.

**Architecture:** Processes and threads are heap-allocated; a process is refcounted by its live thread count, so the address space is freed by whichever thread exits last. `current` and the syscall entry stack move into a `struct cpu` reached through `GS`, fixing two globals that would corrupt silently under SMP. All blocking goes through one interruptible wait-queue primitive.

**Tech Stack:** C (gnu11, freestanding, `-mcmodel=kernel`), NASM, x86-64, GRUB/Multiboot2, QEMU for verification.

**Spec:** `docs/superpowers/specs/2026-08-27-threads-milestone-design.md`
**Roadmap:** `docs/superpowers/specs/2026-08-27-roadmap-architecture-design.md`

## Global Constraints

- **No host test runner.** This is bare-metal code with no host runtime. Every verification is an in-kernel selftest or userland test program, run under headless QEMU, checked by grepping the serial log. Never write or propose host unit tests.
- **QEMU never exits on its own.** Always wrap it in `timeout`.
- **Every verification needs a fresh disk image.** `fat16_write_selftest` creates `/NEWDIR`, so a stale image reports `FAILED` on the second and later boots. Always `rm -f build/disk.img build/disk2.img` before `make disk-image`.
- **Work happens directly on `main`.** No feature branches (project convention, `CLAUDE.md`).
- **Standard-library convention is binding** (`CLAUDE.md`): any kernel feature reachable from user mode ships with a `lib/` wrapper **and** a `docs/stdlib.md` update in the same task.
- **Syscall numbers and shared structs are duplicated by hand** between `kernel/` and `lib/` — the two trees share no headers. Anything added to one must be added to the other in the same task.
- QEMU line for this milestone (AVX does not arrive until milestone 2):
  `-cpu Nehalem -boot order=d -cdrom build/neoos.iso -drive file=build/disk.img,format=raw -drive file=build/disk2.img,format=raw -display none -no-reboot`

### The "boot log UNCHANGED" check

Tasks 2, 3 and 5 are refactors that must not change observable
behavior. Each is gated on the boot log's **deterministic subset**
being unchanged.

**Corrected during execution of Task 2.** A byte-identical comparison
does not work: two runs of the *identical* binary differ by ~57 lines,
because looper/yielder preemption interleaves differently every run,
and even `task exited` lines reorder relative to `vfstest` output. The
gate therefore (a) filters out timing-dependent lines and (b) compares
the **sorted multiset**, so line ordering may vary but the set of
lines may not. Verified: with this filter, two runs of one binary
differ by 0 lines.

```bash
filter() {
  grep -vE '^\[timer\] tick=|calibrated lapic|kmain address=|^\[(looper|yielder) pid=[0-9]+\] tick$' "$1"
}

# BEFORE starting such a task, capture a baseline:
rm -f build/disk.img build/disk2.img
make clean && make iso disk-image
timeout 60 qemu-system-x86_64 -cpu Nehalem -boot order=d \
  -cdrom build/neoos.iso -drive file=build/disk.img,format=raw \
  -drive file=build/disk2.img,format=raw -display none -no-reboot \
  -serial file:/tmp/baseline.log
filter /tmp/baseline.log | sort > /tmp/baseline.txt

# after the task, same run into /tmp/after.log, then:
filter /tmp/after.log | sort > /tmp/after.txt
diff /tmp/baseline.txt /tmp/after.txt && echo "IDENTICAL"
```

`kmain address=` is filtered because adding code moves it, and
`free_frames=` because changing `.bss` moves it (Task 3 removes the
static `struct task tasks[16]`, freeing exactly 16KB = 4 frames).
Neither is a behavior change; check any `free_frames` delta is
explainable rather than ignoring it. **Any other diff is a bug in the task, not an
acceptable change.**

### Note on sequencing

The spec lists wait queues before the process/thread split. This plan
swaps them: wait queues are written **after** the split (Task 4), so
`waitq` blocks a `struct thread` directly instead of being written
against `struct task` and immediately rewritten. Everything else
follows the spec's order.

---

## File Structure

**Created:**

| File | Responsibility |
|---|---|
| `kernel/lock.h` / `lock.c` | spinlocks, mutexes, lock-rank checker |
| `kernel/cpu_local.h` / `cpu_local.c` | `struct cpu`, GS setup, per-CPU accessors |
| `kernel/waitq.h` / `waitq.c` | wait queues, interruptible sleep |
| `kernel/sched/proc.h` / `proc.c` | process lifecycle (Task 5 move) |
| `kernel/sched/thread.h` / `thread.c` | thread lifecycle (Task 5 move) |
| `kernel/sched/sched.c` | scheduler, run queue, idle (Task 5 move) |
| `lib/include/thread.h` / `lib/thread.c` | userland thread API |
| `userland/threadtest.c` | the milestone's proof program |

**Modified:** `kernel/process.c`, `kernel/process.h`, `kernel/syscall.c`, `kernel/syscall_entry.asm`, `kernel/isr.asm`, `kernel/tss.c`, `kernel/tss.h`, `kernel/gdt.c`, `kernel/kernel.c`, `lib/syscall.c`, `docs/stdlib.md`, `Makefile`.

---

## Task 1: Spinlocks and the lock-rank checker

**Files:**
- Create: `kernel/lock.h`, `kernel/lock.c`
- Modify: `kernel/kernel.c` (call the selftest)

**Interfaces:**
- Produces: `struct spinlock`; `spin_init(struct spinlock *, uint8_t rank, const char *name)`; `uint64_t spin_lock_irqsave(struct spinlock *)`; `void spin_unlock_irqrestore(struct spinlock *, uint64_t flags)`; `int lock_rank_ok(uint8_t rank)`; `int lock_held_depth(void)`; `void lock_selftest(void)`; the `LOCK_RANK_*` constants.
- Consumes: nothing.

The rank stack is a file-static here and moves into `struct cpu` in
Task 2. That is correct while exactly one CPU exists.

- [ ] **Step 1: Create `kernel/lock.h`**

```c
#ifndef NEOOS_LOCK_H
#define NEOOS_LOCK_H

#include <stdint.h>

// Lock ranks from the roadmap architecture spec. Acquisition must be
// strictly ascending: a lock may only be taken while every lock
// already held has a STRICTLY LOWER rank. Rank 0 is outermost.
#define LOCK_RANK_PROCTABLE   0
#define LOCK_RANK_PROCESS     1
#define LOCK_RANK_THREAD      2
#define LOCK_RANK_MOUNTTABLE  3
#define LOCK_RANK_VNODEHASH   4
#define LOCK_RANK_VNODE       5
#define LOCK_RANK_BLOCKDEV    6
#define LOCK_RANK_DRIVER      7
#define LOCK_RANK_RUNQUEUE    8
#define LOCK_RANK_HEAP        9
#define LOCK_RANK_PMM        10

#define LOCK_MAX_HELD 8

struct spinlock {
    volatile uint32_t locked;
    uint8_t     rank;
    const char *name;
};

void     spin_init(struct spinlock *l, uint8_t rank, const char *name);
uint64_t spin_lock_irqsave(struct spinlock *l);
void     spin_unlock_irqrestore(struct spinlock *l, uint64_t flags);

// Returns 1 if acquiring `rank` right now would be legal on this CPU.
// Exists so the selftest can prove the checker detects an inversion
// without actually triggering the panic.
int lock_rank_ok(uint8_t rank);

// Number of spinlocks currently held by this CPU. Used by the mutex
// code (Task 4) to refuse to sleep with a spinlock held.
int lock_held_depth(void);

void lock_selftest(void);

#endif
```

- [ ] **Step 2: Create `kernel/lock.c`**

```c
#include "lock.h"
#include "serial.h"

// Held-rank stack for this CPU. Moves into struct cpu in Task 2; a
// single global is correct while exactly one CPU exists.
static uint8_t held_ranks[LOCK_MAX_HELD];
static int     held_depth;

static void lock_panic(const char *msg, const char *a, const char *b) {
    __asm__ volatile ("cli");
    serial_write_string("[lock] PANIC: ");
    serial_write_string(msg);
    serial_write_string(" acquiring=");
    serial_write_string(a ? a : "(null)");
    serial_write_string(" holding=");
    serial_write_string(b ? b : "(none)");
    serial_write_string("\n");
    for (;;) { __asm__ volatile ("hlt"); }
}

void spin_init(struct spinlock *l, uint8_t rank, const char *name) {
    l->locked = 0;
    l->rank   = rank;
    l->name   = name;
}

int lock_held_depth(void) { return held_depth; }

int lock_rank_ok(uint8_t rank) {
    if (held_depth == 0) { return 1; }
    return rank > held_ranks[held_depth - 1];
}

uint64_t spin_lock_irqsave(struct spinlock *l) {
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) :: "memory");

    if (!lock_rank_ok(l->rank)) {
        lock_panic("rank inversion", l->name, "see previous acquire");
    }
    if (held_depth >= LOCK_MAX_HELD) {
        lock_panic("held-lock stack overflow", l->name, 0);
    }

    // Uncontended on one CPU, but a real atomic so the SMP milestone
    // changes nothing here.
    while (__atomic_exchange_n(&l->locked, 1u, __ATOMIC_ACQUIRE)) {
        __asm__ volatile ("pause");
    }

    held_ranks[held_depth++] = l->rank;
    return flags;
}

void spin_unlock_irqrestore(struct spinlock *l, uint64_t flags) {
    if (held_depth <= 0) {
        lock_panic("unlock with nothing held", l->name, 0);
    }
    held_depth--;
    __atomic_store_n(&l->locked, 0u, __ATOMIC_RELEASE);
    if (flags & (1ULL << 9)) {
        __asm__ volatile ("sti");
    }
}

void lock_selftest(void) {
    struct spinlock outer, inner;
    spin_init(&outer, LOCK_RANK_PROCESS, "selftest-outer");
    spin_init(&inner, LOCK_RANK_RUNQUEUE, "selftest-inner");

    if (lock_held_depth() != 0) {
        serial_write_string("[lock] selftest FAILED: depth not 0 at entry\n");
        return;
    }

    uint64_t f1 = spin_lock_irqsave(&outer);
    if (lock_held_depth() != 1) {
        serial_write_string("[lock] selftest FAILED: depth after one acquire\n");
        return;
    }
    // Ascending is legal, descending is not -- checked WITHOUT
    // acquiring, so the panic path is never entered.
    if (!lock_rank_ok(LOCK_RANK_RUNQUEUE)) {
        serial_write_string("[lock] selftest FAILED: ascending rank rejected\n");
        return;
    }
    if (lock_rank_ok(LOCK_RANK_PROCTABLE)) {
        serial_write_string("[lock] selftest FAILED: inversion not detected\n");
        return;
    }
    if (lock_rank_ok(LOCK_RANK_PROCESS)) {
        serial_write_string("[lock] selftest FAILED: equal rank accepted\n");
        return;
    }

    uint64_t f2 = spin_lock_irqsave(&inner);
    if (lock_held_depth() != 2) {
        serial_write_string("[lock] selftest FAILED: depth after two acquires\n");
        return;
    }
    spin_unlock_irqrestore(&inner, f2);
    spin_unlock_irqrestore(&outer, f1);

    if (lock_held_depth() != 0) {
        serial_write_string("[lock] selftest FAILED: depth not 0 at exit\n");
        return;
    }
    if (outer.locked != 0 || inner.locked != 0) {
        serial_write_string("[lock] selftest FAILED: lock still held\n");
        return;
    }
    serial_write_string("[lock] selftest passed\n");
}
```

- [ ] **Step 3: Call the selftest from `kmain`**

In `kernel/kernel.c`, add `#include "lock.h"` with the other includes,
and call `lock_selftest();` immediately after `heap_init()`'s selftest
line (before `ata` initialization), so it runs before anything depends
on it.

- [ ] **Step 4: Build and verify**

```bash
make build 2>&1 | grep -iE "error|warning"
rm -f build/disk.img build/disk2.img
make clean && make iso disk-image
timeout 60 qemu-system-x86_64 -cpu Nehalem -boot order=d \
  -cdrom build/neoos.iso -drive file=build/disk.img,format=raw \
  -drive file=build/disk2.img,format=raw -display none -no-reboot \
  -serial file:/tmp/neoos.log
grep -n "\[lock\]" /tmp/neoos.log
grep -c "FAILED\|exception" /tmp/neoos.log
```

Expected: `[lock] selftest passed`, and zero `FAILED`/exceptions.

- [ ] **Step 5: Commit**

```bash
git add kernel/lock.h kernel/lock.c kernel/kernel.c
git commit -m "Add spinlocks with an ascending-rank lock-order checker"
```

---

## Task 2: Per-CPU data and `swapgs`

**Files:**
- Create: `kernel/cpu_local.h`, `kernel/cpu_local.c`
- Modify: `kernel/syscall_entry.asm`, `kernel/isr.asm`, `kernel/tss.h`, `kernel/tss.c`, `kernel/gdt.c`, `kernel/process.c`, `kernel/kernel.c`, `kernel/lock.c`

**Interfaces:**
- Consumes: `LOCK_MAX_HELD` from Task 1.
- Produces: `struct cpu`; `void cpu_local_init(void)`; `struct cpu *this_cpu(void)`; the `CPU_*` byte-offset constants used by assembly.

This task changes no behavior. It is gated on the boot log being
identical — **capture the baseline first** (see Global Constraints).

- [ ] **Step 1: Capture the baseline boot log**

Run the baseline capture from the Global Constraints section into
`/tmp/baseline.txt`. Do this before editing anything.

- [ ] **Step 2: Create `kernel/cpu_local.h`**

`current` is typed `void *` here on purpose: this task must not depend
on the process/thread split, which is Task 3. Task 3 retypes it.

```c
#ifndef NEOOS_CPU_LOCAL_H
#define NEOOS_CPU_LOCAL_H

#include <stdint.h>
#include <stddef.h>
#include "lock.h"
#include "tss.h"

#define MAX_CPUS 1   // raised by the SMP milestone

// Per-CPU block, reached through GS. Byte offsets below are consumed
// by syscall_entry.asm and isr.asm and are asserted against the real
// struct layout, so drift fails the build instead of corrupting a
// register at runtime.
#define CPU_SELF      0
#define CPU_CURRENT   8
#define CPU_IDLE     16
#define CPU_TSS      24
#define CPU_USER_RSP 32
#define CPU_KSTACK   40

struct cpu {
    struct cpu       *self;             // gs:0 -- this block's own address
    void             *current;          // retyped to struct thread * in Task 3
    void             *idle;             // retyped to struct thread * in Task 3
    struct tss_entry *tss;
    uint64_t          user_rsp_scratch; // was a global in syscall_entry.asm
    uint64_t          kernel_stack;     // mirrors tss->rsp0 for the syscall path
    uint32_t          lapic_id;
    int               held_depth;
    uint8_t           held_ranks[LOCK_MAX_HELD];
};

_Static_assert(offsetof(struct cpu, self)             == CPU_SELF,     "CPU_SELF");
_Static_assert(offsetof(struct cpu, current)          == CPU_CURRENT,  "CPU_CURRENT");
_Static_assert(offsetof(struct cpu, idle)             == CPU_IDLE,     "CPU_IDLE");
_Static_assert(offsetof(struct cpu, tss)              == CPU_TSS,      "CPU_TSS");
_Static_assert(offsetof(struct cpu, user_rsp_scratch) == CPU_USER_RSP, "CPU_USER_RSP");
_Static_assert(offsetof(struct cpu, kernel_stack)     == CPU_KSTACK,   "CPU_KSTACK");

extern struct cpu cpus[MAX_CPUS];

// Sets IA32_GS_BASE to this CPU's block and IA32_KERNEL_GS_BASE to 0
// (userland's GS value). Call once per CPU, before the first syscall
// or interrupt that uses GS.
void cpu_local_init(void);

static inline struct cpu *this_cpu(void) {
    struct cpu *c;
    __asm__ volatile ("mov %%gs:0, %0" : "=r"(c));
    return c;
}

#endif
```

- [ ] **Step 3: Create `kernel/cpu_local.c`**

```c
#include "cpu_local.h"
#include "serial.h"

#define MSR_GS_BASE        0xC0000101
#define MSR_KERNEL_GS_BASE 0xC0000102

struct cpu cpus[MAX_CPUS];

static void wrmsr64(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)value;
    uint32_t hi = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

void cpu_local_init(void) {
    struct cpu *c = &cpus[0];
    c->self             = c;
    c->current          = 0;
    c->idle             = 0;
    c->tss              = &tss[0];
    c->user_rsp_scratch = 0;
    c->kernel_stack     = 0;
    c->lapic_id         = 0;
    c->held_depth       = 0;

    // While executing in the kernel, GS_BASE names this block and
    // KERNEL_GS_BASE holds userland's GS value (0 -- NeoOS gives
    // userland no GS base). Every kernel entry swapgs's them into
    // that arrangement and every exit swaps them back.
    wrmsr64(MSR_GS_BASE, (uint64_t)(uintptr_t)c);
    wrmsr64(MSR_KERNEL_GS_BASE, 0);

    serial_write_string("[cpu] per-CPU block installed\n");
}
```

- [ ] **Step 4: Make the TSS an array**

In `kernel/tss.h`, replace `extern struct tss_entry tss;` with an
array. Do **not** include `cpu_local.h` here — `cpu_local.h` includes
`tss.h`, and the reverse include would be circular.

```c
#define MAX_TSS 1   // one per CPU; raised by the SMP milestone
extern struct tss_entry tss[MAX_TSS];
```

In `kernel/tss.c`:

```c
struct tss_entry tss[MAX_TSS];
static unsigned char ist1_stack[IST1_STACK_SIZE] __attribute__((aligned(16)));

void tss_init(void) {
    unsigned char *raw = (unsigned char *)&tss[0];
    for (unsigned int i = 0; i < sizeof(struct tss_entry); i++) {
        raw[i] = 0;
    }
    tss[0].ist1 = (uint64_t)(ist1_stack + IST1_STACK_SIZE);
    tss[0].iomap_base = sizeof(struct tss_entry);
}
```

In `kernel/gdt.c:32`, change `(uint64_t)&tss` to `(uint64_t)&tss[0]`.

In `kernel/process.c`'s `schedule()`, change `tss.rsp0 = next->kernel_stack_top;` to also update the per-CPU mirror:

```c
    struct cpu *c = this_cpu();
    c->tss->rsp0     = next->kernel_stack_top;
    c->kernel_stack  = next->kernel_stack_top;
```

and add `#include "cpu_local.h"` to `process.c`.

- [ ] **Step 5: Call `cpu_local_init` from `kmain`**

In `kernel/kernel.c`, add `#include "cpu_local.h"`, and call
`cpu_local_init();` **immediately after `tss_init()` and before
`syscall_init()`** — the per-CPU block must exist before any syscall
or interrupt reads `GS`.

- [ ] **Step 6: Convert `syscall_entry.asm`**

Replace the `extern tss` line and the `section .bss` block entirely
(the `user_rsp_scratch` global is deleted), and change the two ends of
the function. The full head becomes:

```nasm
extern syscall_dispatch

; Per-CPU block offsets -- must match kernel/cpu_local.h's CPU_*
; defines, which are _Static_assert'd against struct cpu's layout.
CPU_USER_RSP equ 32
CPU_KSTACK   equ 40

section .text
[bits 64]
global syscall_entry

syscall_entry:
    ; SYSCALL leaves RSP on the USER stack. swapgs brings this CPU's
    ; per-CPU block into GS (userland's GS value goes to
    ; IA32_KERNEL_GS_BASE), giving us a scratch slot and the kernel
    ; stack pointer without touching any register the caller owns.
    swapgs
    mov [gs:CPU_USER_RSP], rsp
    mov rsp, [gs:CPU_KSTACK]

    push qword [gs:CPU_USER_RSP] ; user RSP
    push rcx                     ; user RIP
    push r11                     ; user RFLAGS
```

(the rest of the pushes, `sti`, the argument shuffle and the `call`
are unchanged), and the tail becomes:

```nasm
    pop r11
    pop rcx
    pop qword [gs:CPU_USER_RSP]
    mov rsp, [gs:CPU_USER_RSP]
    swapgs

    o64 sysret
```

- [ ] **Step 7: Convert `isr.asm` with a conditional `swapgs`**

An interrupt taken from ring 0 must **not** `swapgs`, or `GS` ends up
holding userland's value inside the kernel. The interrupted `CS` is on
the stack; its low two bits are the CPL.

On entry, the stack is `[rsp]=vector [rsp+8]=errcode [rsp+16]=RIP
[rsp+24]=CS`. Change `isr_common_stub`'s head to:

```nasm
isr_common_stub:
    ; [rsp]=vector [rsp+8]=error_code [rsp+16]=RIP [rsp+24]=CS.
    ; CPL is CS[1:0]; 3 means the interrupt came from user mode and GS
    ; must be swapped. Swapping unconditionally would point GS at
    ; userland's value for interrupts taken in the kernel.
    test byte [rsp+24], 3
    jz .no_swapgs_in
    swapgs
.no_swapgs_in:
    push rax
```

and its tail — after `add rsp, 16` the layout is `[rsp]=RIP
[rsp+8]=CS`:

```nasm
    add rsp, 16     ; drop vector_number + error_code
    test byte [rsp+8], 3
    jz .no_swapgs_out
    swapgs
.no_swapgs_out:
    iretq
```

Also fix the stale comment at the top of the file: it claims "no ring
3 exists yet", which stopped being true in the processes milestone.
Replace that paragraph with:

```nasm
; In long mode the CPU always pushes SS and RSP, at every privilege
; level, so the frame layout is uniform. Entries from ring 3 need
; swapgs; entries from ring 0 must not have it -- see isr_common_stub.
```

- [ ] **Step 8: Move the rank stack into `struct cpu`**

In `kernel/lock.c`, delete the `held_ranks`/`held_depth` file-statics,
add `#include "cpu_local.h"`, and route every use through the per-CPU
block:

```c
int lock_held_depth(void) { return this_cpu()->held_depth; }

int lock_rank_ok(uint8_t rank) {
    struct cpu *c = this_cpu();
    if (c->held_depth == 0) { return 1; }
    return rank > c->held_ranks[c->held_depth - 1];
}
```

and in `spin_lock_irqsave` / `spin_unlock_irqrestore` replace
`held_depth`/`held_ranks` with `c->held_depth`/`c->held_ranks` after
taking `struct cpu *c = this_cpu();`.

**Ordering hazard:** `lock_selftest()` now reads `GS`, so `kmain` must
call `cpu_local_init()` before `lock_selftest()`. Move the
`lock_selftest();` call from Task 1 to sit immediately after
`cpu_local_init();`.

- [ ] **Step 8b: Give GS to userland in the ring-3 trampolines**

**Added during execution — the plan originally missed this, and the
task cannot boot without it.** Loading a GS *selector* (`mov gs, ax`)
zeroes `IA32_GS_BASE` as a side effect. Two consequences:

1. `kernel_thread_trampoline` (`context_switch.asm`) and
   `fork_trampoline` (`fork_trampoline.asm`) `iretq` into ring 3 with
   no `swapgs`, so `GS_BASE` would still name the per-CPU block while
   userland runs; the first interrupt from ring 3 then swaps it
   *away*, leaving `GS_BASE = 0`. Observed: `#GP` in `schedule()` with
   `rax=0xf000ff53f000ff53` — the BIOS data area at physical 0.
2. `gdt_flush.asm` also does `mov gs, ax`, so `cpu_local_init()` must
   run **after** `gdt_init()`, not before. Installing the base earlier
   has it wiped a few instructions later.

In both trampolines, insert `cli` before the segment loads, drop
`mov gs, <sel>` from the group, then after the remaining loads:

```nasm
    swapgs              ; GS_BASE <- userland's 0, KERNEL_GS_BASE <- per-CPU
    mov gs, ax          ; selector only; base is already 0
```

`mov gs` must come *after* `swapgs`, or the per-CPU pointer is
destroyed before it reaches `IA32_KERNEL_GS_BASE`. Interrupts must be
off across the `swapgs`..`iretq` window: GS already holds userland's
value there while the CPU is still at CPL0, so `isr_common_stub`'s
conditional `swapgs` would correctly decline to swap it back. `iretq`
restores `IF` from the pushed RFLAGS.

In `kmain`, order is `tss_init(); gdt_init(); cpu_local_init();
lock_selftest();`.

- [ ] **Step 9: Build and verify the log is unchanged**

```bash
make build 2>&1 | grep -iE "error|warning"
rm -f build/disk.img build/disk2.img
make clean && make iso disk-image
timeout 60 qemu-system-x86_64 -cpu Nehalem -boot order=d \
  -cdrom build/neoos.iso -drive file=build/disk.img,format=raw \
  -drive file=build/disk2.img,format=raw -display none -no-reboot \
  -serial file:/tmp/after.log
grep -v '^\[timer\] tick=' /tmp/after.log > /tmp/after.txt
diff /tmp/baseline.txt /tmp/after.txt
```

Expected: the only diff is the two new lines `[cpu] per-CPU block
installed` and the relocated `[lock] selftest passed`. Everything else
— all selftests, `[vfstest] ALL PASSED`, `[parent] child exit code=42`,
every `task exited` line with the same pids — must match exactly.

A crash here is almost certainly the conditional `swapgs`: if the
kernel triple-faults on the first timer interrupt, the `test byte`
offset is wrong; if it faults on the first syscall, check that
`CPU_USER_RSP`/`CPU_KSTACK` in the `.asm` match `cpu_local.h`.

- [ ] **Step 10: Commit**

```bash
git add kernel/cpu_local.h kernel/cpu_local.c kernel/syscall_entry.asm \
        kernel/isr.asm kernel/tss.h kernel/tss.c kernel/gdt.c \
        kernel/process.c kernel/kernel.c kernel/lock.c \
        kernel/context_switch.asm kernel/fork_trampoline.asm
git commit -m "Add per-CPU data reached through GS and remove two SMP-fatal globals"
```

---

## Task 3: The process/thread split

**Files:**
- Modify: `kernel/process.h`, `kernel/process.c`, `kernel/syscall.c`, `kernel/fs/vfs.h`, `kernel/fs/vfs.c`, `kernel/cpu_local.h`

**Interfaces:**
- Consumes: `struct cpu`, `this_cpu()` from Task 2.
- Produces: `struct process`, `struct thread`; `struct thread *current_thread(void)`; `struct process *current_proc(void)`; `struct thread *thread_alloc_kernel(void (*entry)(void))`; `struct process *spawn(const char *path)`; `struct thread *fork_task(struct syscall_frame *)`; `int exec_task(const char *, struct syscall_frame *)`; `void thread_exit_self(int code)`; `void process_exit(int code)`; `int64_t wait_for_pid(int pid)`; `void proc_get(struct process *)`; `void proc_put(struct process *)`.

The largest task. Behavior must not change: every process still has
exactly one thread. Gated on the boot log.

**Idle threads get tid 0** and do not consume the id counter, so every
existing pid in the boot log keeps its current value — without this
the log shifts by one and the diff gate becomes useless.

- [ ] **Step 1: Capture a fresh baseline**

Same procedure as Task 2 Step 1, into `/tmp/baseline.txt`. Task 2's
two new lines are part of the baseline now.

- [ ] **Step 2: Rewrite `kernel/process.h`**

```c
#ifndef NEOOS_PROCESS_H
#define NEOOS_PROCESS_H

#include <stdint.h>
#include "cpu.h"
#include "lock.h"
#include "fs/vfs.h"

#define MAX_OPEN_FILES 16
#define KERNEL_STACK_ORDER 2 // 4 frames = 16KiB

// Mirrors syscall_entry.asm's saved-register block exactly, in
// increasing-address order. Unchanged by the thread split.
struct syscall_frame {
    uint64_t r9, r8, r10, rdx, rsi, rdi;
    uint64_t r15, r14, r13, r12, rbp, rbx;
    uint64_t r11;       // user RFLAGS
    uint64_t rcx;       // user RIP
    uint64_t user_rsp;
};

enum thread_state { THREAD_UNUSED, THREAD_READY, THREAD_RUNNING,
                    THREAD_BLOCKED, THREAD_ZOMBIE };

struct file_descriptor {
    int in_use;
    struct vnode *vn;
    uint32_t position;
    int writable;
};

struct waitq; // waitq.h, Task 4

struct process {
    int pid, parent_pid;
    uint32_t refcount;              // == live (non-zombie) thread count
    uint64_t pml4_phys;             // 0 = shares the kernel address space
    struct file_descriptor files[MAX_OPEN_FILES];
    struct thread *threads;         // list via thread->proc_next
    struct thread *zombies;         // exited, unjoined; freed at reap
    uint16_t stack_slots;           // bitmap of live thread user stacks
    int exiting;
    int exit_code;
    enum { PROC_ALIVE, PROC_ZOMBIE } state;
    struct process *next;           // global process list
};

struct thread {
    int tid;
    struct process *proc;
    enum thread_state state;
    uint64_t saved_rsp;
    uint64_t kernel_stack_top;
    uint64_t kernel_stack_phys;
    int stack_slot;                 // -1 for kernel threads
    int kill_pending;
    int exit_code;
    struct waitq *blocked_on;       // Task 4
    uint8_t fpu_state[FPU_STATE_SIZE] __attribute__((aligned(16)));
    struct thread *proc_next;       // sibling list
    struct thread *next;            // run queue link
};

void process_init(void);
void schedule(void);

struct thread  *current_thread(void);
struct process *current_proc(void);

// Kernel-mode-only thread sharing the kernel address space. Used by
// early milestone tests; real processes come from spawn().
struct thread *thread_alloc_kernel(void (*entry)(void));

#define USER_STACK_PAGES 4
#define USER_STACK_TOP 0x0000700000000000ULL

struct process *spawn(const char *path);
struct thread  *fork_task(struct syscall_frame *frame);
int             exec_task(const char *path, struct syscall_frame *frame);

// Ends the calling thread only. When it is the last live thread of its
// process, the address space is freed and waiters are woken.
void thread_exit_self(int code) __attribute__((noreturn));

// Ends the whole process. Task 8 makes this kill siblings; until then
// it is thread_exit_self plus setting proc->exiting.
void process_exit(int code) __attribute__((noreturn));

int64_t wait_for_pid(int pid);

void proc_get(struct process *p);
void proc_put(struct process *p);

#endif
```

- [ ] **Step 3: Retype the per-CPU `current`/`idle` fields**

In `kernel/cpu_local.h`, replace the two `void *` fields with real
types and add the forward declaration:

```c
struct thread;

struct cpu {
    struct cpu       *self;
    struct thread    *current;
    struct thread    *idle;
    ...
```

The offsets and `_Static_assert`s are unchanged — both were pointers
already.

- [ ] **Step 4: Rewrite allocation and lookup in `process.c`**

Replace the `tasks[]` array, `alloc_task_slot`, and `current` with
heap allocation and a global list. Add `#include "mm/heap.h"` if not
already present.

```c
static struct process *proc_list;
static struct spinlock proc_lock;   // rank LOCK_RANK_PROCTABLE
static int next_id = 1;

struct thread  *current_thread(void) { return this_cpu()->current; }
struct process *current_proc(void)   { struct thread *t = current_thread();
                                       return t ? t->proc : 0; }

static int alloc_id(void) {
    uint64_t f = spin_lock_irqsave(&proc_lock);
    int id = next_id++;
    spin_unlock_irqrestore(&proc_lock, f);
    return id;
}

// kmalloc slots are carved from page-aligned pages at multiples of
// their size class, and every class is >= 16, so a returned pointer is
// always at least 16-byte aligned -- which fxsave on thread->fpu_state
// requires.
static struct thread *thread_alloc(struct process *p) {
    struct thread *t = (struct thread *)kmalloc(sizeof(struct thread));
    if (!t) { return 0; }
    for (unsigned i = 0; i < sizeof(struct thread); i++) {
        ((uint8_t *)t)[i] = 0;
    }
    t->tid        = alloc_id();
    t->proc       = p;
    t->state      = THREAD_READY;
    t->stack_slot = -1;
    cpu_default_fpu_state(t->fpu_state);
    if (p) {
        t->proc_next = p->threads;
        p->threads   = t;
        p->refcount++;
    }
    return t;
}

static struct process *proc_alloc(void) {
    struct process *p = (struct process *)kmalloc(sizeof(struct process));
    if (!p) { return 0; }
    for (unsigned i = 0; i < sizeof(struct process); i++) {
        ((uint8_t *)p)[i] = 0;
    }
    p->pid   = alloc_id();
    p->state = PROC_ALIVE;

    uint64_t f = spin_lock_irqsave(&proc_lock);
    p->next   = proc_list;
    proc_list = p;
    spin_unlock_irqrestore(&proc_lock, f);
    return p;
}

static struct process *proc_find(int pid) {
    for (struct process *p = proc_list; p; p = p->next) {
        if (p->pid == pid) { return p; }
    }
    return 0;
}
```

`process_init` becomes:

```c
void process_init(void) {
    spin_init(&proc_lock, LOCK_RANK_PROCTABLE, "proc_list");
    proc_list = 0;
    this_cpu()->current = 0;
    serial_write_string("[process] initialized\n");
}
```

- [ ] **Step 5: Add the idle thread and rewrite `schedule()`**

The idle thread removes `schedule()`'s "nothing ready" special case.
It gets **tid 0** so it does not shift any existing pid.

Add to `process.c`:

```c
static void idle_entry(void) {
    for (;;) { __asm__ volatile ("sti; hlt"); }
}

// Called from process_init's caller once the heap is up. tid 0 is
// reserved for idle threads so they never consume a pid, which keeps
// every other process's pid identical to before this refactor.
static void idle_init(void) {
    struct thread *t = thread_alloc_kernel(idle_entry);
    t->tid   = 0;
    t->state = THREAD_READY;
    // Never on the ready queue: schedule() falls back to it explicitly.
    dequeue_specific(t);
    this_cpu()->idle = t;
}
```

`thread_alloc_kernel` enqueues, so `idle_init` removes it again;
implement `dequeue_specific(struct thread *)` as a singly-linked-list
removal over `ready_head`/`ready_tail`.

`schedule()` keeps its existing `pushfq/cli` prologue and
`schedule_restore_if` epilogue verbatim — including the long comment
at `process.c:141` explaining the reentrancy bug, which still applies.
Change only the body:

```c
    struct thread *next = dequeue_ready();
    if (!next) {
        struct thread *cur = this_cpu()->current;
        if (cur && cur->state == THREAD_RUNNING) {
            schedule_restore_if(flags);
            return;                       // nothing else ready
        }
        next = this_cpu()->idle;          // current is blocked or dead
    }

    struct thread *prev = this_cpu()->current;
    if (prev && prev->state == THREAD_RUNNING && prev != this_cpu()->idle) {
        prev->state = THREAD_READY;
        enqueue_ready(prev);
    }

    next->state = THREAD_RUNNING;
    this_cpu()->current = next;
    this_cpu()->tss->rsp0    = next->kernel_stack_top;
    this_cpu()->kernel_stack = next->kernel_stack_top;

    uint64_t next_cr3 = (next->proc && next->proc->pml4_phys)
                      ? next->proc->pml4_phys
                      : (uint64_t)(uintptr_t)p4_table;
    __asm__ volatile ("mov %0, %%cr3" :: "r"(next_cr3) : "memory");
```

The rest (the `prev == next` early return, `fpu_save`/`fpu_restore`,
`context_switch`) is unchanged apart from the type.

Call `idle_init();` at the end of `process_init()`.

- [ ] **Step 6: Convert `spawn`, `fork_task`, `exec_task`**

`spawn` allocates a process **and** its first thread; the fd table and
`pml4_phys` move to the process:

```c
struct process *spawn(const char *path) {
    uint64_t pml4_phys, entry;
    if (!build_user_address_space(path, &pml4_phys, &entry)) {
        return 0;
    }

    struct process *p = proc_alloc();
    if (!p) {
        serial_write_string("[process] spawn FAILED: out of memory for process\n");
        free_address_space(pml4_phys);
        return 0;
    }
    p->pml4_phys  = pml4_phys;
    p->parent_pid = current_proc() ? current_proc()->pid : 0;

    struct thread *t = thread_alloc(p);
    if (!t) {
        serial_write_string("[process] spawn FAILED: out of memory for thread\n");
        free_address_space(pml4_phys);
        return 0;
    }
    t->stack_slot = 0;
    p->stack_slots = 1;   // slot 0 is the main thread's stack

    uint64_t kstack_phys = pmm_alloc(KERNEL_STACK_ORDER);
    zero_frames(kstack_phys, KERNEL_STACK_ORDER);
    uint64_t kstack_top = (uint64_t)(uintptr_t)phys_to_virt(kstack_phys)
                        + (PMM_FRAME_SIZE << KERNEL_STACK_ORDER);

    uint64_t *sp = (uint64_t *)kstack_top;
    *(--sp) = USER_STACK_TOP;
    *(--sp) = entry;
    *(--sp) = (uint64_t)kernel_thread_trampoline;
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;

    t->saved_rsp         = (uint64_t)sp;
    t->kernel_stack_top  = kstack_top;
    t->kernel_stack_phys = kstack_phys;

    vfs_open_into("/dev/CONSOLE", p, 0, 0);
    vfs_open_into("/dev/CONSOLE", p, 1, 1);
    vfs_open_into("/dev/CONSOLE", p, 2, 1);

    enqueue_ready(t);
    return p;
}
```

`fork_task` creates a new process with one thread, copying the fd
table from `current_proc()` and the register frame as today. The COW
page-table walk (`fork_duplicate_user_pages`) is unchanged; only the
struct plumbing moves. Keep the `child->files[i].vn->refcount++` loop
exactly as it is at `process.c:492`, retargeted at `child_proc->files`.

`exec_task` operates on `current_proc()`: the CR3-before-free ordering
at `process.c:344` is unchanged and still load-bearing.

- [ ] **Step 7: Split exit into thread and process halves**

```c
// Frees the address space when the last live thread leaves. Never
// called on a thread's own kernel stack -- that is freed at join or
// reap (see thread_exit_self).
void proc_put(struct process *p) {
    if (--p->refcount > 0) { return; }

    if (p->pml4_phys) {
        // Leave the dying address space BEFORE freeing it: the buddy
        // allocator writes free-list links into the freed block, so
        // freeing a live PML4 overwrites pml4[0] -- the identity map
        // pmm dereferences those links through. See the full comment
        // this replaces at the old process.c:519.
        __asm__ volatile ("mov %0, %%cr3"
                          :: "r"((uint64_t)(uintptr_t)p4_table) : "memory");
        free_address_space(p->pml4_phys);
        p->pml4_phys = 0;
    }

    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (p->files[i].in_use && p->files[i].vn) {
            vnode_put(p->files[i].vn);
            p->files[i].vn = 0;
            p->files[i].in_use = 0;
        }
    }

    p->state = PROC_ZOMBIE;
    wake_pid_waiters(p->pid);   // replaced by a waitq in Task 4
}

void proc_get(struct process *p) { p->refcount++; }

void thread_exit_self(int code) {
    struct thread *t = current_thread();
    struct process *p = t->proc;

    t->exit_code = code;
    t->state     = THREAD_ZOMBIE;

    // Cannot free our own kernel stack -- we are running on it. Park
    // on the process's zombie list; thread_join (Task 7) or
    // wait_for_pid's reap frees it.
    t->proc_next = p->zombies;
    p->zombies   = t;

    proc_put(p);

    schedule();
    for (;;) { __asm__ volatile ("hlt"); } // unreachable
}

void process_exit(int code) {
    struct process *p = current_proc();
    p->exiting   = 1;
    p->exit_code = code;
    serial_write_string("[process] task exited, pid=");
    serial_write_hex64((uint64_t)p->pid);
    serial_write_string(" code=");
    serial_write_hex64((uint64_t)(int64_t)code);
    serial_write_string("\n");
    thread_exit_self(code);   // Task 8 kills siblings first
}
```

The `[process] task exited` line must keep its exact current wording
and hex formatting — the boot-log diff depends on it.

`wake_pid_waiters(int pid)` is the existing scan from
`process.c:546`, retargeted at threads; Task 4 deletes it.

- [ ] **Step 8: Rewrite `wait_for_pid` for the new reap**

```c
int64_t wait_for_pid(int pid) {
    struct process *p = proc_find(pid);
    if (!p) { return -1; }

    while (p->state != PROC_ZOMBIE) {
        current_thread()->state = THREAD_BLOCKED;
        current_thread()->kill_pending = 0;
        schedule();
    }

    int code = p->exit_code;

    // Free every zombie thread's kernel stack and struct, then the
    // process itself. Safe here: none of them is running.
    struct thread *z = p->zombies;
    while (z) {
        struct thread *next = z->proc_next;
        pmm_free(z->kernel_stack_phys, KERNEL_STACK_ORDER);
        kfree(z);
        z = next;
    }
    p->zombies = 0;

    uint64_t f = spin_lock_irqsave(&proc_lock);
    struct process **pp = &proc_list;
    while (*pp && *pp != p) { pp = &(*pp)->next; }
    if (*pp) { *pp = p->next; }
    spin_unlock_irqrestore(&proc_lock, f);

    kfree(p);
    return code;
}
```

- [ ] **Step 9: Update the callers**

- `kernel/syscall.c`: `current_task()` → `current_thread()`;
  `current_task()->files[...]` → `current_proc()->files[...]`;
  `task_exit(code)` → `process_exit(code)`; `spawn()` now returns
  `struct process *`, so `SYS_SPAWN` returns `p->pid`; `fork_task()`
  returns `struct thread *`, so `SYS_FORK` returns
  `child->proc->pid`.
- `kernel/fs/vfs.h` / `vfs.c`: `vfs_open_into` takes a
  `struct process *` instead of a `struct task *`; update the
  forward declaration and the fd-table accesses.
- `kernel/timer.c` and `kernel/isr.c`: any `struct task` reference
  becomes `struct thread`.

- [ ] **Step 9b: Fix kmalloc's alignment before heap-allocating threads**

**Added during execution — Task 3 cannot boot without it.** The plan
originally asserted that "kmalloc slots are >= 16-byte aligned". That
is **false**: `heap.c` carves slots starting at
`sizeof(struct heap_page)` = 24 bytes, so every `kmalloc` pointer is
8 mod 16. `fxsave`/`fxrstor` `#GP` on a non-16-byte-aligned address,
so the first `schedule()` faulted on `fxrstor (%rax)` with
`rax=0xffff800000432868`.

Fix it at the allocator, not in `thread_alloc`, and align to 64 rather
than 16 so milestone 2's `XSAVE` needs no further change:

```c
struct heap_page {
    struct heap_page *next;
    struct heap_free_slot *free_list;
    uint32_t size_class;
    uint32_t meta;
} __attribute__((aligned(64)));
```

Every size class is a power of two >= 16, so a 64-byte first-slot
offset makes every slot at least 16-byte aligned, and classes >= 64
fully 64-byte aligned. Large allocations get a 64-byte header too.

- [ ] **Step 9c: Keep pids stable — two ID-allocation rules**

**Added during execution.** Two separate off-by-ones shift every pid
and make the log gate useless:

1. A process and its first thread both drawing from `next_id` makes
   pids come out 2, 4, 6, ... Fix: **a process's first thread takes
   the pid as its tid**, matching Linux (main thread `tid == pid`).
   Later threads draw fresh ids, so a tid still never collides with a
   pid.

```c
    t->tid = (p && !p->threads) ? p->pid : alloc_id();
```

2. The idle thread consuming an id before `PARENT` shifts pids by one.
   Fix: **start `next_id` at 0**, so `idle_init()` -- the first
   allocation of all -- naturally takes id 0, the value reserved for
   idle threads. Do NOT allocate an id and then overwrite `tid` with
   0; the counter has already advanced by then.

- [ ] **Step 10: Build and verify the log is unchanged**

```bash
make build 2>&1 | grep -iE "error|warning"
rm -f build/disk.img build/disk2.img
make clean && make iso disk-image
timeout 60 qemu-system-x86_64 -cpu Nehalem -boot order=d \
  -cdrom build/neoos.iso -drive file=build/disk.img,format=raw \
  -drive file=build/disk2.img,format=raw -display none -no-reboot \
  -serial file:/tmp/after.log
grep -v '^\[timer\] tick=' /tmp/after.log > /tmp/after.txt
diff /tmp/baseline.txt /tmp/after.txt && echo IDENTICAL
```

Expected: `IDENTICAL`. Every pid, every `task exited` line, every
selftest, and `[vfstest] ALL PASSED` must match. A pid that shifted by
one means the idle thread consumed an id — check `idle_init` sets
`t->tid = 0` **after** `thread_alloc_kernel`.

- [ ] **Step 11: Commit**

```bash
git add kernel/process.h kernel/process.c kernel/syscall.c \
        kernel/fs/vfs.h kernel/fs/vfs.c kernel/cpu_local.h \
        kernel/isr.c kernel/kernel.c kernel/mm/heap.c
git commit -m "Split struct task into refcounted process and thread"
```

---

## Task 4: Wait queues and mutexes

**Files:**
- Create: `kernel/waitq.h`, `kernel/waitq.c`
- Modify: `kernel/lock.h`, `kernel/lock.c`, `kernel/process.c`, `kernel/process.h`, `kernel/syscall.c`, `kernel/kernel.c`, `kernel/errno.h`

**Interfaces:**
- Consumes: `struct thread`, `schedule()`, `spin_lock_irqsave`.
- Produces: `struct waitq`; `void waitq_init(struct waitq *)`; `int waitq_sleep(struct waitq *q, struct spinlock *release)`; `void waitq_wake_one(struct waitq *)`; `void waitq_wake_all(struct waitq *)`; `struct mutex`; `void mutex_init(struct mutex *, uint8_t rank, const char *name)`; `void mutex_lock(struct mutex *)`; `void mutex_unlock(struct mutex *)`; `void waitq_selftest(void)`.

- [ ] **Step 1: Create `kernel/waitq.h`**

```c
#ifndef NEOOS_WAITQ_H
#define NEOOS_WAITQ_H

#include <stdint.h>
#include "lock.h"

struct thread;

struct waitq {
    struct thread *head, *tail;
};

void waitq_init(struct waitq *q);

// Blocks the calling thread on `q`. If `release` is non-null it is
// unlocked (with `flags`) before blocking and re-locked before
// returning. Returns 0 on a normal wake, -EINTR if the thread was
// killed while blocked.
//
// The lost-wakeup window between releasing `release` and switching
// away is closed by running with interrupts off, which is sufficient
// on one CPU: no waker can run. The SMP milestone replaces these
// internals with a lock handoff WITHOUT changing this signature.
int  waitq_sleep(struct waitq *q, struct spinlock *release);
void waitq_wake_one(struct waitq *q);
void waitq_wake_all(struct waitq *q);

// Removes `t` from whatever queue it is blocked on and makes it ready.
// Used by thread_kill (Task 8).
void waitq_remove(struct thread *t);

void waitq_selftest(void);

#endif
```

- [ ] **Step 2: Create `kernel/waitq.c`**

```c
#include "waitq.h"
#include "process.h"
#include "cpu_local.h"
#include "errno.h"
#include "serial.h"

void waitq_init(struct waitq *q) { q->head = 0; q->tail = 0; }

static void waitq_enqueue(struct waitq *q, struct thread *t) {
    t->next = 0;
    if (q->tail) { q->tail->next = t; } else { q->head = t; }
    q->tail = t;
}

static struct thread *waitq_dequeue(struct waitq *q) {
    struct thread *t = q->head;
    if (t) {
        q->head = t->next;
        if (!q->head) { q->tail = 0; }
        t->next = 0;
    }
    return t;
}

void waitq_remove(struct thread *t) {
    struct waitq *q = t->blocked_on;
    if (!q) { return; }
    struct thread **pp = &q->head;
    struct thread *prev = 0;
    while (*pp && *pp != t) { prev = *pp; pp = &(*pp)->next; }
    if (*pp) {
        *pp = t->next;
        if (q->tail == t) { q->tail = prev; }
    }
    t->next = 0;
    t->blocked_on = 0;
}

int waitq_sleep(struct waitq *q, struct spinlock *release) {
    struct thread *t = current_thread();

    // Interrupts are already off: the caller holds `release`, which
    // spin_lock_irqsave cleared IF for. If there is no lock, clear it
    // here. Either way nothing can wake us between enqueue and
    // schedule().
    uint64_t own_flags = 0;
    if (!release) {
        __asm__ volatile ("pushfq; pop %0; cli" : "=r"(own_flags) :: "memory");
    }

    if (t->kill_pending) {
        if (!release && (own_flags & (1ULL << 9))) { __asm__ volatile ("sti"); }
        return -EINTR;
    }

    waitq_enqueue(q, t);
    t->blocked_on = q;
    t->state      = THREAD_BLOCKED;

    if (release) { spin_unlock_irqrestore(release, 0); } // keep IF off

    schedule();

    // Resumed: either woken normally or killed.
    t->blocked_on = 0;
    int rc = t->kill_pending ? -EINTR : 0;

    if (release) { (void)spin_lock_irqsave(release); }
    else if (own_flags & (1ULL << 9)) { __asm__ volatile ("sti"); }
    return rc;
}

static void waitq_make_ready(struct thread *t) {
    t->blocked_on = 0;
    t->state = THREAD_READY;
    thread_enqueue_ready(t);
}

void waitq_wake_one(struct waitq *q) {
    struct thread *t = waitq_dequeue(q);
    if (t) { waitq_make_ready(t); }
}

void waitq_wake_all(struct waitq *q) {
    struct thread *t;
    while ((t = waitq_dequeue(q)) != 0) { waitq_make_ready(t); }
}
```

`thread_enqueue_ready(struct thread *)` is `process.c`'s existing
`enqueue_ready`, made non-static and declared in `process.h`.

`waitq.c` references `-EINTR`, which `kernel/errno.h` does not define
yet. Add it there now (Task 7 adds the remaining codes):

```c
#define EINTR 4
```

- [ ] **Step 3: Add mutexes to `lock.h` / `lock.c`**

In `lock.h`:

```c
#include "waitq.h"

struct mutex {
    int          locked;
    struct waitq waiters;
    struct spinlock guard;   // rank == the mutex's own rank
    uint8_t      rank;
    const char  *name;
};

void mutex_init(struct mutex *m, uint8_t rank, const char *name);
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);
```

In `lock.c`:

```c
void mutex_init(struct mutex *m, uint8_t rank, const char *name) {
    m->locked = 0;
    m->rank   = rank;
    m->name   = name;
    waitq_init(&m->waiters);
    spin_init(&m->guard, rank, name);
}

void mutex_lock(struct mutex *m) {
    // Sleeping with a spinlock held would deadlock every other CPU
    // once SMP lands, and hides an ordering bug even on one CPU.
    if (lock_held_depth() != 0) {
        lock_panic("mutex taken while holding a spinlock", m->name, 0);
    }
    uint64_t f = spin_lock_irqsave(&m->guard);
    while (m->locked) {
        waitq_sleep(&m->waiters, &m->guard);
    }
    m->locked = 1;
    spin_unlock_irqrestore(&m->guard, f);
}

void mutex_unlock(struct mutex *m) {
    uint64_t f = spin_lock_irqsave(&m->guard);
    m->locked = 0;
    waitq_wake_one(&m->waiters);
    spin_unlock_irqrestore(&m->guard, f);
}
```

Make `lock_panic` non-static and declare it in `lock.h`.

- [ ] **Step 4: Convert `fs_lock` to a mutex**

In `kernel/syscall.c`, delete the spin-wait `fs_lock` at lines 62–83
and replace with:

```c
static struct mutex fs_lock;
#define fs_lock_acquire() mutex_lock(&fs_lock)
#define fs_lock_release() mutex_unlock(&fs_lock)
```

Initialise it in `syscall_init()`:

```c
    mutex_init(&fs_lock, LOCK_RANK_MOUNTTABLE, "fs");
```

Every existing `fs_lock_acquire()`/`fs_lock_release()` call site is
unchanged.

- [ ] **Step 5: Convert `wait_for_pid` and `proc_put` to the waitq**

Add `struct waitq exit_waiters;` to `struct process` (initialise it in
`proc_alloc` with `waitq_init(&p->exit_waiters)`).

In `proc_put`, replace `wake_pid_waiters(p->pid);` with
`waitq_wake_all(&p->exit_waiters);` and delete `wake_pid_waiters`
entirely.

In `wait_for_pid`, replace the `while (p->state != PROC_ZOMBIE)`
spin-and-schedule loop with:

```c
    while (p->state != PROC_ZOMBIE) {
        waitq_sleep(&p->exit_waiters, 0);
    }
```

- [ ] **Step 6: Add the selftest**

Append to `waitq.c`:

```c
static struct waitq selftest_q;
static volatile int selftest_stage;

static void selftest_sleeper(void) {
    selftest_stage = 1;
    waitq_sleep(&selftest_q, 0);
    selftest_stage = 2;
    thread_exit_self(0);
}

void waitq_selftest(void) {
    waitq_init(&selftest_q);
    selftest_stage = 0;

    struct thread *t = thread_alloc_kernel(selftest_sleeper);
    if (!t) {
        serial_write_string("[waitq] selftest FAILED: thread_alloc_kernel\n");
        return;
    }

    // Let it run until it blocks.
    while (selftest_stage == 0) { schedule(); }
    if (selftest_q.head != t) {
        serial_write_string("[waitq] selftest FAILED: sleeper not queued\n");
        return;
    }
    if (t->state != THREAD_BLOCKED) {
        serial_write_string("[waitq] selftest FAILED: sleeper not BLOCKED\n");
        return;
    }

    waitq_wake_one(&selftest_q);
    if (selftest_q.head != 0) {
        serial_write_string("[waitq] selftest FAILED: queue not empty after wake\n");
        return;
    }
    while (selftest_stage != 2) { schedule(); }

    serial_write_string("[waitq] selftest passed\n");
}
```

Call `waitq_selftest();` from `kmain` immediately after
`process_init();` (it needs threads, so it cannot run earlier).

- [ ] **Step 7: Build and verify**

```bash
make build 2>&1 | grep -iE "error|warning"
rm -f build/disk.img build/disk2.img
make clean && make iso disk-image
timeout 60 qemu-system-x86_64 -cpu Nehalem -boot order=d \
  -cdrom build/neoos.iso -drive file=build/disk.img,format=raw \
  -drive file=build/disk2.img,format=raw -display none -no-reboot \
  -serial file:/tmp/neoos.log
grep -n "\[waitq\]\|\[lock\]" /tmp/neoos.log
grep -c "FAILED\|exception" /tmp/neoos.log
grep -n "vfstest\|child exit code" /tmp/neoos.log
```

Expected: `[waitq] selftest passed`, `[lock] selftest passed`, zero
`FAILED`/exceptions, `[parent] child exit code=42`, and
`[vfstest] ALL PASSED`. The full boot log is no longer expected to be
byte-identical — it gains the waitq selftest line — but every prior
line must still be present.

- [ ] **Step 8: Commit**

```bash
git add kernel/waitq.h kernel/waitq.c kernel/lock.h kernel/lock.c \
        kernel/process.c kernel/process.h kernel/syscall.c kernel/kernel.c \
        kernel/errno.h
git commit -m "Add interruptible wait queues and sleeping mutexes"
```

---

## Task 5: Move the scheduler into `kernel/sched/`

**Files:**
- Move: `kernel/process.c` → `kernel/sched/proc.c`, `kernel/sched/thread.c`, `kernel/sched/sched.c`
- Move: `kernel/process.h` → `kernel/sched/proc.h`
- Modify: `Makefile`, and every file that includes `process.h`

**Interfaces:**
- Consumes/produces: unchanged. This task is pure motion.

Pure file motion, gated on the boot log. Doing it separately keeps
Task 3's semantic diff readable — the same reason the VFS milestone
moved `fat16.c` → `fatfs.c` in its own task.

- [ ] **Step 1: Capture a fresh baseline** (Task 2 Step 1 procedure).

- [ ] **Step 2: Move the files**

```bash
mkdir -p kernel/sched
git mv kernel/process.h kernel/sched/proc.h
git mv kernel/process.c kernel/sched/proc.c
```

Then split `proc.c` by responsibility, moving whole functions
unchanged:

- `kernel/sched/sched.c` — `enqueue_ready`, `dequeue_ready`,
  `dequeue_specific`, `thread_enqueue_ready`, `schedule`,
  `schedule_restore_if`, `idle_entry`, `idle_init`.
- `kernel/sched/thread.c` — `thread_alloc`, `thread_alloc_kernel`,
  `thread_exit_self`, `zero_frames`.
- `kernel/sched/proc.c` — `proc_alloc`, `proc_find`, `proc_get`,
  `proc_put`, `process_init`, `build_user_address_space`, `spawn`,
  `fork_task`, `fork_duplicate_user_pages`, `exec_task`,
  `process_exit`, `wait_for_pid`.

Shared file-statics (`proc_list`, `proc_lock`, `next_id`,
`ready_head`, `ready_tail`) become non-static with declarations in
`proc.h`.

- [ ] **Step 3: Update includes**

Replace `#include "process.h"` with `#include "sched/proc.h"` in
`kernel/syscall.c`, `kernel/kernel.c`, `kernel/timer.c`,
`kernel/isr.c`, and `kernel/waitq.c`. In `kernel/fs/vfs.h` and
`vfs.c` use `#include "../sched/proc.h"`.

- [ ] **Step 4: Add `kernel/sched/` to the Makefile globs**

```makefile
C_SOURCES := $(wildcard kernel/*.c) $(wildcard kernel/mm/*.c) \
             $(wildcard kernel/fs/*.c) $(wildcard kernel/sched/*.c)
C_HEADERS := $(wildcard kernel/*.h) $(wildcard kernel/mm/*.h) \
             $(wildcard kernel/fs/*.h) $(wildcard kernel/sched/*.h)
```

The existing pattern rule already does `mkdir -p $(dir $@)`, so
`build/sched/` is created automatically.

- [ ] **Step 5: Build and verify the log is unchanged**

Same commands as Task 3 Step 10. Expected: `IDENTICAL`.

- [ ] **Step 6: Commit**

```bash
git add -A kernel Makefile
git commit -m "Move the scheduler into kernel/sched and split it by responsibility"
```

---

## Task 6: Thread user stacks with guard pages

**Files:**
- Modify: `kernel/sched/proc.h`, `kernel/sched/proc.c`, `kernel/sched/thread.c`

**Interfaces:**
- Produces: `int thread_stack_alloc(struct process *p, uint64_t *out_top)`; `void thread_stack_free(struct process *p, int slot)`; `THREAD_STACK_STRIDE`, `MAX_THREADS_PER_PROC`.

- [ ] **Step 1: Add the layout constants to `proc.h`**

```c
#define MAX_THREADS_PER_PROC 16

// Each thread's user stack is USER_STACK_PAGES pages, followed (at
// lower addresses) by one unmapped guard page, so a stack overflow
// faults instead of silently writing into the next thread's stack.
// Slot 0 is the main thread, at USER_STACK_TOP, so existing single-
// threaded layout is unchanged.
#define THREAD_STACK_STRIDE ((uint64_t)(USER_STACK_PAGES + 1) * PMM_FRAME_SIZE)

static inline uint64_t thread_stack_top_for(int slot) {
    return USER_STACK_TOP - (uint64_t)slot * THREAD_STACK_STRIDE;
}
```

- [ ] **Step 2: Implement slot allocation in `proc.c`**

```c
int thread_stack_alloc(struct process *p, uint64_t *out_top) {
    int slot = -1;
    for (int i = 0; i < MAX_THREADS_PER_PROC; i++) {
        if (!(p->stack_slots & (1u << i))) { slot = i; break; }
    }
    if (slot < 0) { return -1; }

    uint64_t top = thread_stack_top_for(slot);
    uint64_t *pml4 = (uint64_t *)phys_to_virt(p->pml4_phys);

    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t frame = pmm_alloc(0);
        if (!frame) {
            // Unwind the pages already mapped for this slot.
            for (int j = 0; j < i; j++) {
                uint64_t v = top - (uint64_t)(USER_STACK_PAGES - j) * PMM_FRAME_SIZE;
                paging_unmap_from(pml4, v, 1);
            }
            return -1;
        }
        zero_frames(frame, 0);
        uint64_t vaddr = top - (uint64_t)(USER_STACK_PAGES - i) * PMM_FRAME_SIZE;
        paging_map_into(pml4, vaddr, frame,
                        PAGE_WRITABLE | PAGE_NO_EXECUTE | PAGE_USER);
    }
    // The guard page below this stack is simply never mapped.

    p->stack_slots |= (1u << slot);
    *out_top = top;
    return slot;
}

void thread_stack_free(struct process *p, int slot) {
    if (slot < 0) { return; }
    uint64_t top = thread_stack_top_for(slot);
    uint64_t *pml4 = (uint64_t *)phys_to_virt(p->pml4_phys);
    for (int i = 0; i < USER_STACK_PAGES; i++) {
        uint64_t v = top - (uint64_t)(USER_STACK_PAGES - i) * PMM_FRAME_SIZE;
        paging_unmap_from(pml4, v, 1);
    }
    p->stack_slots &= ~(1u << slot);
}
```

`paging_unmap_from(uint64_t *pml4, uint64_t virt, int free_frame)`
does not exist yet. Add it to `kernel/mm/paging.c`/`paging.h`: walk to
the PTE, clear it, `invlpg` the address, and `pmm_free(phys, 0)` when
`free_frame` is set. Return 0 if the page was not mapped.

- [ ] **Step 3: Route `build_user_address_space` through slot 0**

Replace the inline user-stack loop in `build_user_address_space` (the
`for (int i = 0; i < USER_STACK_PAGES; i++)` block) with nothing —
`spawn` now calls `thread_stack_alloc` after the process exists, so
slot 0 gets its stack (and its guard page) from the same code path
every other thread uses.

In `spawn`, after `p->pml4_phys = pml4_phys;`:

```c
    uint64_t user_stack_top;
    int slot = thread_stack_alloc(p, &user_stack_top);
    if (slot != 0) {
        serial_write_string("[process] spawn FAILED: user stack\n");
        free_address_space(pml4_phys);
        return 0;
    }
```

and use `user_stack_top` instead of `USER_STACK_TOP` when building the
initial kernel stack frame. Delete the `p->stack_slots = 1;` line
added in Task 3 — `thread_stack_alloc` now sets it.

`exec_task` resets the layout: after installing the new address space,
set `p->stack_slots = 0` and call `thread_stack_alloc` for slot 0.

- [ ] **Step 4: Build and verify**

```bash
make build 2>&1 | grep -iE "error|warning"
rm -f build/disk.img build/disk2.img
make clean && make iso disk-image
timeout 60 qemu-system-x86_64 -cpu Nehalem -boot order=d \
  -cdrom build/neoos.iso -drive file=build/disk.img,format=raw \
  -drive file=build/disk2.img,format=raw -display none -no-reboot \
  -serial file:/tmp/neoos.log
grep -c "FAILED\|exception" /tmp/neoos.log
grep -n "vfstest\|child exit code\|task exited" /tmp/neoos.log
```

Expected: zero `FAILED`/exceptions, `[vfstest] ALL PASSED`,
`[parent] child exit code=42`, all processes exiting normally. The
main thread's stack is now one page lower in the address space than
before (it gains a guard page below it), which no test observes.

- [ ] **Step 5: Commit**

```bash
git add kernel/sched/proc.h kernel/sched/proc.c kernel/sched/thread.c \
        kernel/mm/paging.h kernel/mm/paging.c
git commit -m "Allocate thread user stacks from a slot bitmap with guard pages"
```

---

## Task 7: Thread syscalls and the userland API

**Files:**
- Create: `lib/include/thread.h`, `lib/thread.c`
- Modify: `kernel/sched/thread.c`, `kernel/sched/proc.h`, `kernel/syscall.c`, `lib/syscall.c`, `docs/stdlib.md`

**Interfaces:**
- Produces (kernel): `struct thread *thread_create(uint64_t entry, uint64_t arg)`; `int thread_join(int tid, int *out_code)`.
- Produces (userland): `thread_create`, `thread_exit`, `thread_join`, `thread_self`, `getdents`-style raw wrappers.

- [ ] **Step 1: Implement `thread_create` in `kernel/sched/thread.c`**

```c
// Starts a new thread in the current process at user RIP `entry` with
// RDI = `arg`, on its own user stack. Returns 0 if no slot, no memory,
// or the process is exiting.
struct thread *thread_create(uint64_t entry, uint64_t arg) {
    struct process *p = current_proc();
    if (p->exiting) { return 0; }

    uint64_t user_stack_top;
    int slot = thread_stack_alloc(p, &user_stack_top);
    if (slot < 0) { return 0; }

    struct thread *t = thread_alloc(p);
    if (!t) { thread_stack_free(p, slot); return 0; }
    t->stack_slot = slot;

    uint64_t kstack_phys = pmm_alloc(KERNEL_STACK_ORDER);
    if (!kstack_phys) {
        thread_stack_free(p, slot);
        return 0;
    }
    zero_frames(kstack_phys, KERNEL_STACK_ORDER);
    uint64_t kstack_top = (uint64_t)(uintptr_t)phys_to_virt(kstack_phys)
                        + (PMM_FRAME_SIZE << KERNEL_STACK_ORDER);

    // Same layout kernel_thread_trampoline expects in spawn(), plus
    // the argument: it pops entry_rip and user_rsp, and we hand RDI
    // through a third slot (see the trampoline change below).
    uint64_t *sp = (uint64_t *)kstack_top;
    *(--sp) = arg;
    *(--sp) = user_stack_top;
    *(--sp) = entry;
    *(--sp) = (uint64_t)kernel_thread_trampoline;
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;
    *(--sp) = 0; *(--sp) = 0; *(--sp) = 0;

    t->saved_rsp         = (uint64_t)sp;
    t->kernel_stack_top  = kstack_top;
    t->kernel_stack_phys = kstack_phys;

    thread_enqueue_ready(t);
    return t;
}
```

In `kernel/fork_trampoline.asm`'s `kernel_thread_trampoline`, pop the
extra argument slot into `rdi` before entering ring 3. `spawn` must
push a zero `arg` slot too, so both paths share one layout.

- [ ] **Step 2: Implement `thread_join`**

```c
// Waits for `tid` (a thread of the calling process) to exit, frees its
// kernel stack, user stack and struct, and stores its exit code.
// Returns 0, -ESRCH if no such thread, or -EDEADLK for self-join.
int thread_join(int tid, int *out_code) {
    struct process *p = current_proc();
    if (tid == current_thread()->tid) { return -EDEADLK; }

    for (;;) {
        // Already exited?
        struct thread **pp = &p->zombies;
        while (*pp && (*pp)->tid != tid) { pp = &(*pp)->proc_next; }
        if (*pp) {
            struct thread *z = *pp;
            *pp = z->proc_next;
            if (out_code) { *out_code = z->exit_code; }
            thread_stack_free(p, z->stack_slot);
            pmm_free(z->kernel_stack_phys, KERNEL_STACK_ORDER);
            kfree(z);
            return 0;
        }

        // Still running?
        struct thread *t = p->threads;
        while (t && t->tid != tid) { t = t->proc_next; }
        if (!t) { return -ESRCH; }

        if (waitq_sleep(&p->join_waiters, 0) == -EINTR) { return -EINTR; }
    }
}
```

Add `struct waitq join_waiters;` to `struct process`, initialised in
`proc_alloc`. In `thread_exit_self`, after parking on the zombie list,
add `waitq_wake_all(&p->join_waiters);` and remove the exiting thread
from `p->threads`.

Add `#define ESRCH 3` and `#define EDEADLK 35` to `kernel/errno.h`
and `lib/include/errno.h`. `EINTR` (4) is already in `kernel/errno.h`
from Task 4 but still needs adding to `lib/include/errno.h`.

- [ ] **Step 3: Add the syscalls to `kernel/syscall.c`**

```c
#define SYS_THREAD_CREATE 17
#define SYS_THREAD_EXIT   18
#define SYS_THREAD_JOIN   19
#define SYS_THREAD_SELF   20
```

```c
        case SYS_THREAD_CREATE: {
            struct thread *t = thread_create(a1, a2);
            return t ? t->tid : -EAGAIN;
        }
        case SYS_THREAD_EXIT:
            thread_exit_self((int)a1);
            return 0; // unreachable
        case SYS_THREAD_JOIN: {
            int code = 0;
            int rc = thread_join((int)a1, &code);
            if (rc == 0 && a2) {
                *(int *)(uintptr_t)a2 = code;
            }
            return rc;
        }
        case SYS_THREAD_SELF:
            return current_thread()->tid;
```

Add `#define EAGAIN 11` to both `errno.h` files.

- [ ] **Step 4: Create `lib/include/thread.h`**

```c
#ifndef NEOOS_THREAD_H
#define NEOOS_THREAD_H

typedef int thread_t;

// Starts `fn(arg)` on a new thread in this process. Returns 0 and
// stores the tid in *out, or a negative <errno.h> code.
int thread_create(thread_t *out, void (*fn)(void *), void *arg);

// Ends the calling thread. When it is the last thread of the process,
// the process ends too.
void thread_exit(int code) __attribute__((noreturn));

// Waits for `t` to exit and stores its exit code (if `exit_code` is
// non-null). Returns 0, -ESRCH if no such thread, or -EDEADLK if `t`
// is the calling thread.
int thread_join(thread_t t, int *exit_code);

thread_t thread_self(void);

#endif
```

- [ ] **Step 5: Create `lib/thread.c`**

```c
#include <thread.h>
#include <errno.h>

// The kernel starts a thread at RIP=entry with RDI=arg, but a raw fn
// that simply returned would fall off the end of its stack. So the
// kernel always enters this trampoline, which calls fn and then exits
// the thread properly.
//
// lib/ has no malloc, so the (fn, arg) pairs live in a static table
// sized to the kernel's MAX_THREADS_PER_PROC.
#define MAX_THREAD_SLOTS 16

struct thread_start {
    int    in_use;
    void (*fn)(void *);
    void  *arg;
};

static struct thread_start slots[MAX_THREAD_SLOTS];

extern int  __sys_thread_create(unsigned long entry, unsigned long arg);
extern void __sys_thread_exit(int code) __attribute__((noreturn));
extern int  __sys_thread_join(int tid, int *code);
extern int  __sys_thread_self(void);

static void thread_trampoline(void *raw) {
    struct thread_start *s = (struct thread_start *)raw;
    void (*fn)(void *) = s->fn;
    void  *arg = s->arg;
    s->in_use = 0;
    fn(arg);
    __sys_thread_exit(0);
}

int thread_create(thread_t *out, void (*fn)(void *), void *arg) {
    struct thread_start *s = 0;
    for (int i = 0; i < MAX_THREAD_SLOTS; i++) {
        if (!slots[i].in_use) { s = &slots[i]; break; }
    }
    if (!s) { return -EAGAIN; }
    s->in_use = 1;
    s->fn     = fn;
    s->arg    = arg;

    int tid = __sys_thread_create((unsigned long)(void *)thread_trampoline,
                                  (unsigned long)(void *)s);
    if (tid < 0) { s->in_use = 0; return tid; }
    if (out) { *out = tid; }
    return 0;
}

void thread_exit(int code) { __sys_thread_exit(code); }
int  thread_join(thread_t t, int *exit_code) { return __sys_thread_join(t, exit_code); }
thread_t thread_self(void) { return __sys_thread_self(); }
```

- [ ] **Step 6: Add the raw wrappers to `lib/syscall.c`**

```c
#define SYS_THREAD_CREATE 17
#define SYS_THREAD_EXIT   18
#define SYS_THREAD_JOIN   19
#define SYS_THREAD_SELF   20

int __sys_thread_create(unsigned long entry, unsigned long arg) {
    return (int)syscall2(SYS_THREAD_CREATE, (int64_t)entry, (int64_t)arg);
}

void __sys_thread_exit(int code) {
    syscall1(SYS_THREAD_EXIT, code);
    for (;;) { }   // unreachable
}

int __sys_thread_join(int tid, int *code) {
    return (int)syscall2(SYS_THREAD_JOIN, tid, (int64_t)(uint64_t)(uintptr_t)code);
}

int __sys_thread_self(void) {
    return (int)syscall0(SYS_THREAD_SELF);
}
```

Use whatever `syscall0`/`syscall1`/`syscall2` helpers `lib/syscall.c`
already defines; match the existing style exactly.

- [ ] **Step 7: Document in `docs/stdlib.md`**

Add a `<thread.h>` section after `<dirent.h>`:

```markdown
## `<thread.h>`

- `int thread_create(thread_t *out, void (*fn)(void *), void *arg)` —
  starts `fn(arg)` on a new thread sharing this process's address
  space and file descriptors. Returns 0 and stores the tid in `*out`,
  or `-EAGAIN` if the process already has 16 threads. Each thread gets
  its own 16KiB user stack with an unmapped guard page below it.
- `void thread_exit(int code)` — ends the calling thread. When it is
  the last thread of the process, the process ends too. Never returns.
- `int thread_join(thread_t t, int *exit_code)` — waits for `t` to
  exit and stores its exit code. Returns 0, `-ESRCH` if no such thread
  exists in this process, or `-EDEADLK` if `t` is the caller. Joining
  is the only way to reclaim a thread's stacks before process exit.
- `thread_t thread_self(void)` — the calling thread's tid. TIDs and
  PIDs share one number space, so a tid never equals a pid.
```

Amend the `<unistd.h>` entries for `exit` and `fork`:

```markdown
- `void exit(int code)` — terminates the calling process, including
  every other thread in it, and never returns. Threads blocked in a
  syscall are interrupted rather than left running.
- `int fork(void)` — ... NeoOS-specific: in a multithreaded process
  only the CALLING thread is duplicated; the child starts with exactly
  one thread. This matches POSIX.
```

Add to `<errno.h>`: `ESRCH` (3), `EAGAIN` (11), `EINTR` (4),
`EDEADLK` (35).

- [ ] **Step 8: Build and verify**

```bash
make build 2>&1 | grep -iE "error|warning"
rm -f build/disk.img build/disk2.img
make clean && make iso disk-image
timeout 60 qemu-system-x86_64 -cpu Nehalem -boot order=d \
  -cdrom build/neoos.iso -drive file=build/disk.img,format=raw \
  -drive file=build/disk2.img,format=raw -display none -no-reboot \
  -serial file:/tmp/neoos.log
grep -c "FAILED\|exception" /tmp/neoos.log
grep -n "vfstest\|child exit code" /tmp/neoos.log
```

Expected: the existing boot still passes unchanged — nothing calls the
new syscalls yet. Zero `FAILED`/exceptions.

- [ ] **Step 9: Commit**

```bash
git add kernel/sched/thread.c kernel/sched/proc.h kernel/sched/proc.c \
        kernel/syscall.c kernel/errno.h kernel/fork_trampoline.asm \
        lib/include/thread.h lib/thread.c lib/syscall.c \
        lib/include/errno.h docs/stdlib.md
git commit -m "Add thread_create/join/exit/self syscalls and library API"
```

---

## Task 8: Interruptible waits and `exit()` killing siblings

**Files:**
- Modify: `kernel/sched/thread.c`, `kernel/sched/proc.c`, `kernel/waitq.c`, `kernel/syscall.c`

**Interfaces:**
- Produces: `void thread_kill(struct thread *t)`.

- [ ] **Step 1: Implement `thread_kill`**

In `kernel/sched/thread.c`:

```c
// Marks `t` for termination. A thread blocked in waitq_sleep is
// removed from its queue and made runnable; it observes kill_pending
// and returns -EINTR, then unwinds to thread_exit_self. A running or
// ready thread checks kill_pending on its next syscall return.
void thread_kill(struct thread *t) {
    if (t->state == THREAD_ZOMBIE) { return; }
    t->kill_pending = 1;
    if (t->state == THREAD_BLOCKED) {
        waitq_remove(t);
        t->state = THREAD_READY;
        thread_enqueue_ready(t);
    }
}
```

- [ ] **Step 2: Make `process_exit` kill siblings**

```c
void process_exit(int code) {
    struct process *p = current_proc();
    struct thread *self = current_thread();

    p->exiting   = 1;
    p->exit_code = code;

    serial_write_string("[process] task exited, pid=");
    serial_write_hex64((uint64_t)p->pid);
    serial_write_string(" code=");
    serial_write_hex64((uint64_t)(int64_t)code);
    serial_write_string("\n");

    for (struct thread *t = p->threads; t; t = t->proc_next) {
        if (t != self) { thread_kill(t); }
    }

    thread_exit_self(code);
}
```

The last thread to leave — which may be a killed sibling rather than
the caller — drops `refcount` to zero and frees the address space.

- [ ] **Step 3: Check `kill_pending` on the syscall return path**

In `syscall_dispatch`, immediately before returning, add:

```c
    // A thread killed by a sibling's exit() unwinds here: whatever it
    // was doing has finished, and it must not return to user mode.
    if (current_thread()->kill_pending) {
        thread_exit_self(current_proc()->exit_code);
    }
```

Restructure `syscall_dispatch` so every `return` flows through one
exit point (assign to a local `int64_t ret` and `goto out;`, or wrap
the existing switch in a helper called by a thin outer function). The
second form is less invasive: rename the existing function to
`syscall_dispatch_inner` and add:

```c
int64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2,
                         uint64_t a3, uint64_t a4,
                         struct syscall_frame *frame) {
    int64_t ret = syscall_dispatch_inner(num, a1, a2, a3, a4, frame);
    if (current_thread()->kill_pending) {
        thread_exit_self(current_proc()->exit_code);
    }
    return ret;
}
```

Match `syscall_dispatch`'s existing signature exactly — check it in
`kernel/syscall.c` rather than trusting the sketch above.

- [ ] **Step 4: Make `wait_for_pid` and `thread_join` honour `-EINTR`**

Both already call `waitq_sleep`; ensure both propagate a `-EINTR`
return rather than looping. `wait_for_pid` becomes:

```c
    while (p->state != PROC_ZOMBIE) {
        if (waitq_sleep(&p->exit_waiters, 0) == -EINTR) { return -EINTR; }
    }
```

- [ ] **Step 5: Build and verify**

```bash
make build 2>&1 | grep -iE "error|warning"
rm -f build/disk.img build/disk2.img
make clean && make iso disk-image
timeout 60 qemu-system-x86_64 -cpu Nehalem -boot order=d \
  -cdrom build/neoos.iso -drive file=build/disk.img,format=raw \
  -drive file=build/disk2.img,format=raw -display none -no-reboot \
  -serial file:/tmp/neoos.log
grep -c "FAILED\|exception" /tmp/neoos.log
grep -n "vfstest\|child exit code\|task exited" /tmp/neoos.log
```

Expected: unchanged behavior — every existing process is
single-threaded, so `process_exit` finds no siblings to kill. Zero
`FAILED`/exceptions, `[vfstest] ALL PASSED`,
`[parent] child exit code=42`.

- [ ] **Step 6: Commit**

```bash
git add kernel/sched/thread.c kernel/sched/proc.c kernel/waitq.c kernel/syscall.c
git commit -m "Make exit() terminate sibling threads through interruptible waits"
```

---

## Task 9: `threadtest`, the leak gate, and the final regression

**Files:**
- Create: `userland/threadtest.c`
- Modify: `Makefile`, `kernel/kernel.c`

**Interfaces:**
- Consumes: everything. Produces no new kernel interface.

- [ ] **Step 1: Write `userland/threadtest.c`**

```c
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <thread.h>

static volatile int shared_counter;
static volatile unsigned long worker_local_addr[4];
static volatile int worker_tid[4];

static void worker(void *arg) {
    int idx = (int)(long)arg;
    volatile int local = 0;          // lives on THIS thread's stack
    worker_local_addr[idx] = (unsigned long)(void *)&local;
    worker_tid[idx] = thread_self();

    for (int i = 0; i < 1000; i++) {
        __atomic_fetch_add(&shared_counter, 1, __ATOMIC_SEQ_CST);
    }
    local = idx;
    thread_exit(100 + idx);
}

static int check_shared_address_space(void) {
    shared_counter = 0;
    thread_t t[4];
    for (int i = 0; i < 4; i++) {
        if (thread_create(&t[i], worker, (void *)(long)i) != 0) {
            printf("[threadtest] FAILED: thread_create %d\n", i);
            return 0;
        }
    }
    for (int i = 0; i < 4; i++) {
        int code = -1;
        if (thread_join(t[i], &code) != 0) {
            printf("[threadtest] FAILED: thread_join %d\n", i);
            return 0;
        }
        if (code != 100 + i) {
            printf("[threadtest] FAILED: join %d got code %d\n", i, code);
            return 0;
        }
    }
    if (shared_counter != 4000) {
        printf("[threadtest] FAILED: counter=%d want 4000\n", shared_counter);
        return 0;
    }
    printf("[threadtest] shared address space passed (counter=%d)\n", shared_counter);
    return 1;
}

static int check_separate_stacks(void) {
    for (int i = 0; i < 4; i++) {
        for (int j = i + 1; j < 4; j++) {
            if (worker_local_addr[i] == worker_local_addr[j]) {
                printf("[threadtest] FAILED: threads %d and %d share a stack\n", i, j);
                return 0;
            }
        }
    }
    printf("[threadtest] separate stacks passed\n");
    return 1;
}

static int check_distinct_tids(void) {
    for (int i = 0; i < 4; i++) {
        if (worker_tid[i] == thread_self() || worker_tid[i] == 0) {
            printf("[threadtest] FAILED: bad tid %d\n", worker_tid[i]);
            return 0;
        }
    }
    printf("[threadtest] distinct tids passed\n");
    return 1;
}

static volatile int fd_from_worker = -1;

static void fd_worker(void *arg) {
    (void)arg;
    fd_from_worker = open("/tmp/THREADFD.TXT", O_CREAT | O_RDWR | O_TRUNC);
    thread_exit(0);
}

static int check_shared_fd_table(void) {
    thread_t t;
    if (thread_create(&t, fd_worker, 0) != 0) {
        printf("[threadtest] FAILED: thread_create for fd test\n");
        return 0;
    }
    thread_join(t, 0);
    if (fd_from_worker < 0) {
        printf("[threadtest] FAILED: worker could not open file\n");
        return 0;
    }
    // The fd the worker opened must be usable from THIS thread.
    if (write(fd_from_worker, "shared-fd", 9) != 9) {
        printf("[threadtest] FAILED: inherited fd not writable\n");
        return 0;
    }
    close(fd_from_worker);
    printf("[threadtest] shared fd table passed\n");
    return 1;
}

static void blocker(void *arg) {
    (void)arg;
    // Blocks forever: nothing ever exits pid 99999.
    wait(99999);
    // Reached only if the wait is interrupted, which exit() should
    // never let us observe -- the thread must die inside the syscall.
    printf("[threadtest] FAILED: blocked thread resumed\n");
    thread_exit(1);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int ok = 1;
    ok &= check_shared_address_space();
    ok &= check_separate_stacks();
    ok &= check_distinct_tids();
    ok &= check_shared_fd_table();

    printf("[threadtest] %s\n", ok ? "ALL PASSED" : "SOME CHECKS FAILED");

    // Last: prove exit() kills a thread blocked in a syscall. If this
    // is broken the process never exits and the boot hangs, which the
    // timeout catches.
    thread_t b;
    if (thread_create(&b, blocker, 0) != 0) {
        printf("[threadtest] FAILED: thread_create for blocker\n");
        return 1;
    }
    for (volatile int i = 0; i < 1000000; i++) { } // let it reach the wait
    printf("[threadtest] exiting with a blocked sibling\n");
    return ok ? 0 : 1;
}
```

- [ ] **Step 2: Add the Makefile rule and disk entry**

```makefile
$(USERLAND_BUILD)/THRDTEST.ELF: $(USERLAND_DIR)/threadtest.c $(USERLAND_DIR)/user.ld $(LIB_BUILD)/crt0.o $(LIB_BUILD)/libneoos.a
	mkdir -p $(USERLAND_BUILD)
	$(CC) $(USER_CFLAGS) -T $(USERLAND_DIR)/user.ld -o $@ $(LIB_BUILD)/crt0.o $(USERLAND_DIR)/threadtest.c -L$(LIB_BUILD) -lneoos
```

The name is `THRDTEST.ELF`, not `THREADTEST.ELF`: FAT 8.3 mangles
names longer than eight characters, which already cost this project a
debugging session in the COW milestone.

Add `$(USERLAND_BUILD)/THRDTEST.ELF` to the `$(DISK_IMG)`
prerequisites and, with the other `mcopy` lines:

```makefile
	mcopy -i $(DISK_IMG) $(USERLAND_BUILD)/THRDTEST.ELF ::BIN/THRDTEST.ELF
```

- [ ] **Step 3: Spawn it as a permanent part of the boot**

In `kernel/kernel.c`, after `spawn("/BIN/VFSTEST.ELF");`:

```c
    spawn("/BIN/THRDTEST.ELF");
```

- [ ] **Step 4: Build and verify**

```bash
rm -f build/disk.img build/disk2.img
make clean && make iso disk-image
timeout 90 qemu-system-x86_64 -cpu Nehalem -boot order=d \
  -cdrom build/neoos.iso -drive file=build/disk.img,format=raw \
  -drive file=build/disk2.img,format=raw -display none -no-reboot \
  -serial file:/tmp/neoos.log
grep -n "threadtest" /tmp/neoos.log
grep -c "FAILED\|exception" /tmp/neoos.log
```

Expected, in order: `shared address space passed (counter=4000)`,
`separate stacks passed`, `distinct tids passed`,
`shared fd table passed`, `[threadtest] ALL PASSED`,
`exiting with a blocked sibling`, and then a `[process] task exited`
line for its pid. Zero `FAILED`, zero exceptions.

If the log stops after `exiting with a blocked sibling` and the run
hits the timeout, `thread_kill` is not waking the blocked sibling —
check that `waitq_remove` correctly repairs `q->tail` when removing
the only queued thread.

- [ ] **Step 5: Add the temporary leak gate**

As in the VFS and COW milestones, `wait_for_pid` needs a valid current
thread, so this runs as a kernel thread. Add above `kmain` in
`kernel/kernel.c`:

```c
static void thread_leak_test(void) {
    serial_write_string("[test] before: free_frames=");
    serial_write_hex64(pmm_free_frame_count());
    serial_write_string(" vnodes=");
    serial_write_hex64(vfs_vnode_in_use_count());
    serial_write_string("\n");

    for (int i = 0; i < 5; i++) {
        struct process *p = spawn("/BIN/THRDTEST.ELF");
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

Temporarily replace `kmain`'s six `spawn(...)` calls with
`thread_alloc_kernel(thread_leak_test);`.

- [ ] **Step 6: Run the leak gate**

```bash
rm -f build/disk.img build/disk2.img
make clean && make iso disk-image
timeout 120 qemu-system-x86_64 -cpu Nehalem -boot order=d \
  -cdrom build/neoos.iso -drive file=build/disk.img,format=raw \
  -drive file=build/disk2.img,format=raw -display none -no-reboot \
  -serial file:/tmp/leak.log
grep -n "\[test\]" /tmp/leak.log
grep -c "FAILED\|exception" /tmp/leak.log
grep -c "ALL PASSED" /tmp/leak.log
```

Expected: `vnodes` identical before and after (equal to the mount
count, 4), and five `ALL PASSED`.

`free_frames` may legitimately differ by a small constant — the ramfs
page backing `/tmp/THREADFD.TXT`, created on the first iteration only.
**Confirm it is constant, not growing:** re-run with the loop bound
changed to 10. If the delta is the same at 5 and at 10 it is one-time
allocation; if it doubles, it is a real leak and must be fixed before
this task completes. This is exactly how the VFS milestone's 2-frame
delta was cleared.

- [ ] **Step 7: Revert the leak gate and run the final regression**

Delete `thread_leak_test` and restore `kmain`'s six `spawn(...)`
calls. Confirm `git diff --stat kernel/kernel.c` shows only the
`THRDTEST` line against Task 9 Step 3.

```bash
rm -f build/disk.img build/disk2.img
make clean && make iso disk-image
timeout 90 qemu-system-x86_64 -cpu Nehalem -boot order=d \
  -cdrom build/neoos.iso -drive file=build/disk.img,format=raw \
  -drive file=build/disk2.img,format=raw -display none -no-reboot \
  -serial file:/tmp/neoos.log
grep -c "FAILED\|exception" /tmp/neoos.log
grep -n "selftest\|child exit code\|vfstest\|threadtest\|task exited" /tmp/neoos.log
```

Checked against the spec's success criteria one by one:

- `[lock] selftest passed` and `[waitq] selftest passed`, plus every
  prior milestone's selftest (`pmm`, `paging`, `heap`, `fat16`,
  `fat16 write`, `vfs`).
- `[cpu] per-CPU block installed`.
- `[threadtest] ALL PASSED` with all four checks, and the process
  exiting despite a blocked sibling.
- `[vfstest] ALL PASSED`, `[parent] child exit code=42`, the bursty
  looper interleave and the dense yielder interleave — milestones 5–10
  behavior unchanged.
- Zero `FAILED`, zero exceptions.

- [ ] **Step 8: Commit**

```bash
git add userland/threadtest.c Makefile kernel/kernel.c
git commit -m "Add threadtest proving shared address space, separate stacks, and interruptible exit"
```

---

## Corrections found during execution

Recorded so the next milestone's plan does not repeat them.

1. **`mov gs, <selector>` zeroes `IA32_GS_BASE`** (Task 2). It bit in
   two places: `gdt_flush.asm`, so `cpu_local_init()` must run *after*
   `gdt_init()`; and both ring-3 trampolines, which needed a `swapgs`
   after their segment loads. Symptom: `#GP` in `schedule()` with
   `rax=0xf000ff53f000ff53` (the BIOS data area at physical 0).

2. **The byte-identical log gate was unusable** (Task 2). Two runs of
   one binary differ by ~57 lines. Replaced with a filter plus a
   sorted-multiset comparison, then validated: 0 lines of difference
   between two runs of one binary.

3. **`kmalloc` was not 16-byte aligned** (Task 3). Slots started at
   `sizeof(struct heap_page)` = 24, so every pointer was 8 mod 16 and
   `fxrstor` `#GP`'d. Fixed by aligning the header to 64, which also
   pre-satisfies milestone 2's `XSAVE`.

4. **Two ID off-by-ones shifted every pid** (Task 3). Fixed by giving a
   process's first thread the pid as its tid (Linux convention) and
   starting `next_id` at 0 so idle takes the reserved id 0.

5. **`kmalloc` only ever checked the FRONT page of a size class**
   (Task 9), stranding every slot freed into an older page. Invisible
   until threads made allocation churn; measured as ~1 frame leaked per
   spawn/wait cycle, growing linearly. Fixed by scanning the class's
   page list. Delta afterwards: 3 frames at both 5 and 10 iterations,
   i.e. flat.

6. **The blocked-sibling check did not block** (Task 9). It waited on a
   nonexistent pid, but `wait_for_pid` returns -1 immediately for an
   unknown pid, so the thread only *sometimes* lost the race to print
   its failure. Rewritten to `thread_join` a thread that never exits.

7. **Serial output interleaved on one CPU** (Task 9, beyond plan
   scope). A timer interrupt mid-print let another context split a
   line: `[process] task exited, pid=0x000000000child running...`. The
   roadmap assigns serial locking to the SMP milestone, but the log is
   this project's only debugging channel, so it was fixed here. The
   lock must be **rank-free** (`spin_lock_raw`): serial runs from the
   first line of `kmain`, long before `cpu_local_init()` installs a GS
   base, and the rank checker reads per-CPU state through GS.

## Notes for the implementer

- **The conditional `swapgs` in Task 2 is the highest-risk change in
  this milestone.** A triple fault on the first timer interrupt means
  the `test byte [rsp+24], 3` offset is wrong; a fault on the first
  syscall means the `CPU_*` equates in the `.asm` disagree with
  `cpu_local.h`. The `_Static_assert`s catch the C side only — the
  `equ`s in assembly must be checked by eye.
- **Refcount bugs are this milestone's characteristic failure**, the
  way vnode refcounts were the VFS milestone's. When something
  misbehaves, print `p->refcount` and walk `p->threads` first: a
  process freed while a thread still runs shows up as a page fault in
  `schedule()` right after a `task exited` line.
- **The boot-log diff gate is not optional** for Tasks 2, 3 and 5.
  Skipping it is how the VFS milestone shipped a broken
  `fatfs_create` that the standard boot never exercised.
- **Every verification needs a fresh disk image.**
  `fat16_write_selftest` creates `/NEWDIR`, and a stale image reports
  `FAILED` on the second and later boots. This has already caused one
  false alarm in this project's history.
- **QEMU never exits on its own.** Always wrap it in `timeout`.
- **`kernel/sched/proc.c` will be around 700 lines** after Task 5.
  That is acceptable; if a natural seam appears later (address-space
  construction separating cleanly from process lifecycle), note it for
  a follow-up rather than acting on it mid-milestone.
