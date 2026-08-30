#ifndef NEOOS_LOCK_H
#define NEOOS_LOCK_H

#include <stdint.h>
#include "spinlock_types.h"
#include "waitq.h"

/*
 * Container_of macro: Given a pointer to a member of a struct,
 * calculate the pointer to the containing struct.
 *
 * Usage: struct rcu_head *rh = ...;
 *        struct process *p = container_of(rh, struct process, rcu);
 */
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - __builtin_offsetof(type, member)))

// Lock ranks from docs/superpowers/specs/2026-08-27-roadmap-architecture-design.md.
// Acquisition must be strictly ascending: a lock may only be taken
// while every lock already held has a STRICTLY LOWER rank. Rank 0 is
// outermost. The checker exists because this project has no host test
// runner -- without it, a lock-order inversion is found only by a rare
// deadlock under QEMU, once SMP makes one possible at all.
#define LOCK_RANK_PROCTABLE   0
#define LOCK_RANK_PROCESS     1
#define LOCK_RANK_THREAD      2
// Per-process address space (vma list + pml4). Sits LOW, not up beside
// HEAP where its allocation behaviour would suggest: a demand-paging
// fault takes this lock and then reads through the filesystem, so it
// must be above nothing in VFS -- and such a fault can sleep, so it must
// be below RUNQUEUE. Next to HEAP it inverts on the first mmap-backed
// page fault.
#define LOCK_RANK_MM          3
#define LOCK_RANK_MOUNTTABLE  4
#define LOCK_RANK_VNODEHASH   5
#define LOCK_RANK_VNODE       6
#define LOCK_RANK_BLOCKDEV    7
#define LOCK_RANK_DRIVER      8
// Per-wait-queue. Above every lock legally held across waitq_sleep() (a
// mutex passes its own guard in as `release`, carrying the mutex's
// rank), and below RUNQUEUE, since the sleep path reaches schedule()
// after the queue is updated.
#define LOCK_RANK_WAITQ       9
#define LOCK_RANK_RUNQUEUE   10
#define LOCK_RANK_FDTABLE    11  // file descriptor table (per-bucket locks, after VFS)
#define LOCK_RANK_HEAP       12
#define LOCK_RANK_PMM        13
// The signal-queue pool: a leaf allocator taken while a process's
// sig_lock (rank 1) is held, and holding nothing itself. It sits
// innermost rather than beside sig_lock because equal ranks are an
// inversion -- acquisition must be strictly ascending.
#define LOCK_RANK_SIGQUEUE   14
// Leaf lock: rank above every other, so it is always legal to take and
// can be acquired from anywhere -- including while holding any other
// lock. Whatever holds it must never acquire another lock.
#define LOCK_RANK_SERIAL    255

#define LOCK_MAX_HELD 8

// struct spinlock itself lives in spinlock_types.h so waitq.h can embed
// one without including this header back.

void     spin_init(struct spinlock *l, uint8_t rank, const char *name);
uint64_t spin_lock_irqsave(struct spinlock *l);
void     spin_unlock_irqrestore(struct spinlock *l, uint64_t flags);

// Acquires two locks of the SAME rank. Only legal for the work-stealing
// path, which must hold two run queues at once. Equal ranks are an
// inversion for the normal checker and stay one; safety here comes from
// a consistent global order instead -- both locks are taken in address
// order, so no two CPUs can build a cycle. `a` and `b` must differ.
uint64_t spin_lock_ordered_pair(struct spinlock *a, struct spinlock *b);
void     spin_unlock_ordered_pair(struct spinlock *a, struct spinlock *b,
                                  uint64_t flags);

// Returns 1 if acquiring `rank` right now would be legal on this CPU.
// Exists so the selftest can prove the checker detects an inversion
// without actually triggering the panic.
int lock_rank_ok(uint8_t rank);

// Rank-free acquire/release. Uses no per-CPU state, so it is safe
// before cpu_local_init() has installed a GS base -- which serial
// output needs, since it runs from the very first line of kmain. Only
// for leaf locks that never nest inside another lock.
uint64_t spin_lock_raw(struct spinlock *l);
void     spin_unlock_raw(struct spinlock *l, uint64_t flags);

// Number of spinlocks currently held by this CPU. Used by the mutex
// code to refuse to sleep with a spinlock held.
int lock_held_depth(void);

// Sleeping lock: may block, therefore may NEVER be taken in interrupt
// context or while holding a spinlock. Use for anything that performs
// I/O -- the filesystem lock is the motivating case.
struct mutex {
    int             locked;
    struct waitq    waiters;
    struct spinlock guard;
    uint8_t         rank;
    const char     *name;
};

void mutex_init(struct mutex *m, uint8_t rank, const char *name);
void mutex_lock(struct mutex *m);
void mutex_unlock(struct mutex *m);

void lock_panic(const char *msg, const char *a, const char *b);

void lock_selftest(void);

#endif
