#ifndef NEOOS_CPU_LOCAL_H
#define NEOOS_CPU_LOCAL_H

#include <stdint.h>
#include <stddef.h>
#include "lock.h"
#include "tss.h"

// 128 fits inside xAPIC's 8-bit APIC ID field, so no x2APIC needed.
#define MAX_CPUS 128

// Per-CPU block, reached through GS. The byte offsets below are
// consumed by syscall_entry.asm and isr.asm and are asserted against
// the real struct layout, so drift fails the build instead of
// corrupting a register at runtime. The assembly's own `equ`s must be
// kept in sync BY EYE -- _Static_assert only covers the C side.
#define CPU_SELF      0
#define CPU_CURRENT   8
#define CPU_IDLE     16
#define CPU_TSS      24
#define CPU_USER_RSP 32
#define CPU_KSTACK   40

struct thread;

struct cpu {
    struct cpu       *self;             // gs:0 -- this block's own address
    struct thread    *current;
    struct thread    *idle;
    struct tss_entry *tss;
    uint64_t          user_rsp_scratch; // was a global in syscall_entry.asm
    uint64_t          kernel_stack;     // mirrors tss->rsp0 for the syscall path
    uint32_t          lapic_id;
    volatile uint32_t online;           // set by the AP itself once running
    int               held_depth;
    uint8_t           held_ranks[LOCK_MAX_HELD];

    // Per-CPU ready queue. Phase 7 added the layout; Phase 10 added the
    // lock that makes it safe to touch from a stealing CPU. Declared
    // AFTER kernel_stack so the CPU_* byte offsets asserted below are
    // undisturbed.
    struct spinlock   ready_lock;
    struct thread    *ready_head;
    struct thread    *ready_tail;
    uint32_t          ready_count;      // list length, for steal victim choice
    // The thread this CPU has switched AWAY from but not yet released.
    // Consumed by sched_post_switch(), which runs as the INCOMING
    // thread; see sched.c for why the release cannot happen any
    // earlier.
    struct thread    *prev_pending;
    // Ticks left in the running thread's time slice. Per-CPU because it
    // is a property of what THIS CPU is running; see timer_handler.
    uint32_t          timeslice_remaining;
    // Local timer interrupts taken. Only the selftest reads it, and it
    // is the one direct evidence that this CPU is actually being
    // preempted rather than merely looking busy.
    volatile uint64_t timer_ticks_local;
    // Threads this CPU has taken from another CPU's queue. Read by the
    // steal selftest, which needs a fact about the mechanism rather
    // than a snapshot of where a handful of test threads happened to
    // land.
    volatile uint64_t steals;
    // IA32_FS_BASE as this CPU last wrote it, so a context switch that
    // does not change the thread pointer skips the WRMSR. See
    // schedule().
    uint64_t          fs_base_loaded;
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
void cpu_local_init_bsp(void);
void cpu_local_init_ap(int index);

static inline struct cpu *this_cpu(void) {
    struct cpu *c;
    __asm__ volatile ("mov %%gs:0, %0" : "=r"(c));
    return c;
}

#endif
