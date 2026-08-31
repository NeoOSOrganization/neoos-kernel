#ifndef NEOOS_SCHED_INTERNAL_H
#define NEOOS_SCHED_INTERNAL_H

// Internals shared between kernel/sched/*.c only. The public scheduler
// API the rest of the kernel uses lives in proc.h; nothing outside this
// directory should include this header.

#include "sched/proc.h"

extern uint64_t p4_table[512]; // boot.asm's live PML4

// proc.c
extern struct process  *proc_list;
extern struct spinlock  proc_lock;
int alloc_id(void);

// sched.c (Phase 7: Per-CPU ready queues, no global ready_head/ready_tail)
void enqueue_ready(struct thread *t);
struct thread *dequeue_ready(void);
void idle_init(void);
void idle_init_for(int cpu_index);
void enqueue_ready_on(int cpu_index, struct thread *t);

// Releases the thread this CPU switched away from. Called from
// schedule() itself and -- because a brand-new thread starts at a
// trampoline and never returns through context_switch -- from
// context_switch.asm and fork_trampoline.asm.
void sched_post_switch(void);

// Kernel threads park here on exit; the idle thread reclaims them,
// since they have no parent to reap them and cannot free the stack
// they are running on. Guarded by kzombies_lock (rank
// LOCK_RANK_PROCTABLE) -- thread_exit_self's kernel-thread branch and
// idle_entry's drain hold nothing else when they touch the list.
extern struct thread   *kzombies;
extern struct spinlock  kzombies_lock;

// thread.c
struct thread *thread_alloc(struct process *p);
void zero_frames(uint64_t phys, unsigned order);

#endif
