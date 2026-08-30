#ifndef NEOOS_CPU_LOCAL_H
#define NEOOS_CPU_LOCAL_H

#include <stdint.h>
#include <stddef.h>
#include "lock.h"
#include "tss.h"

#define MAX_CPUS 1   // raised by the SMP milestone

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
