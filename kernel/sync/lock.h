#ifndef NEOOS_LOCK_H
#define NEOOS_LOCK_H

#include <stdint.h>
#include "sync/spinlock_types.h"
#include "sync/waitq.h"

/*
 * Container_of macro: Given a pointer to a member of a struct,
 * calculate the pointer to the containing struct.
 *
 * Usage: struct waitq *q = ...;
 *        struct mutex *m = container_of(q, struct mutex, waiters);
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
// A struct tty's lock (console and every pty slave). Same rank as
// DRIVER -- it is what the old single console_tty lock used, and it is
// handed to waitq_sleep() as `release` so it must sit below WAITQ. No
// two tty locks are ever held at once (a pty master write takes only
// the slave's), so sharing a rank number is safe.
#define LOCK_RANK_TTY         LOCK_RANK_DRIVER
// IPC object guards. Every one of these follows the same pattern the
// mutex established: hold the guard, decide whether to block, and hand
// the guard to waitq_sleep() as `release` so the decision and the sleep
// cannot be separated. They therefore all sit BELOW WAITQ. They are
// distinct ranks rather than one shared rank only because the checker
// treats equal ranks as an inversion; no two of them are ever held at
// once, so the order between them is arbitrary.
#define LOCK_RANK_FUTEX       9
#define LOCK_RANK_PIPE       10
// One rank for the socket layer's per-object guards and one for its
// global bound-socket table. Two, not one, because demux walks the
// table and then locks the socket it found.
// Two ranks, not one: UDP demux walks the bound-socket table and then
// locks the socket it found, so the table must rank strictly outside
// the object. Equal ranks are an inversion to the checker, and rightly.
#define LOCK_RANK_SOCKTABLE  11
#define LOCK_RANK_SOCKET     12
// The timed-sleep list. It was rank THREAD (2), which was fine while
// waitq_sleep_timeout's only caller held no lock -- but any of the IPC
// guards above hands itself to waitq_sleep_timeout as `release`, and
// timeout_add runs while that guard is still held. At rank 2 that is a
// descending acquire and an instant panic. It belongs here, directly
// under WAITQ, for the same reason WAITQ is where it is: it is taken on
// the way into a sleep, under whatever guard the sleeper was holding.
#define LOCK_RANK_TIMEOUT    13
// Per-wait-queue. Above every lock legally held across waitq_sleep() (a
// mutex passes its own guard in as `release`, carrying the mutex's
// rank), and below RUNQUEUE, since the sleep path reaches schedule()
// after the queue is updated.
#define LOCK_RANK_WAITQ      14
#define LOCK_RANK_RUNQUEUE   15
#define LOCK_RANK_FDTABLE    16  // file descriptor table (per-bucket locks, after VFS)
#define LOCK_RANK_HEAP       17
#define LOCK_RANK_PMM        18
// The signal-queue pool: a leaf allocator taken while a process's
// p->lock (rank 1, LOCK_RANK_PROCESS) is held, and holding nothing
// itself. It sits innermost rather than beside LOCK_RANK_PROCESS
// because equal ranks are an inversion -- acquisition must be strictly
// ascending.
#define LOCK_RANK_SIGQUEUE   19
// TLB shootdown bookkeeping: the deferred-free queue is filled from
// paging_unmap_from, which runs UNDER a process's mm_lock (rank 3), so
// it must rank strictly below it. It is a leaf -- tlb_flush_deferred
// releases it before calling pmm_free.
#define LOCK_RANK_TLB        20
// Input subsystem: key event fan-out and grab. Taken from the keyboard
// IRQ (so it must be a leaf-ish rank), but held only during ring-buffer
// append -- never across tty_input_char or waitq_wake calls.
#define LOCK_RANK_INPUT     21
// The kernel virtual terminals: vt_active, each VT's diff cache
// (vc->shown / shown_valid) and kd_mode. Sits ABOVE TTY because the
// write path is tty_obj_write -> t->lock -> vt_backend_output ->
// render_diff, so the VT state is touched while a tty lock is held.
// Sits BELOW FBCON because rendering calls into the con_driver
// underneath it. vt_switch and vt_scroll take t->lock then this, from
// the keyboard IRQ holding nothing else, so the order is the same on
// every path. The panic path (vt_panic_reset) takes NEITHER -- see
// vt.c's panic-mode comment.
#define LOCK_RANK_VT        251
// The CSPRNG pool. A leaf: rand_bytes holds it across arithmetic only,
// and takes nothing under it. It had LOCK_RANK_SERIAL, which is not the
// deliberate TTY==DRIVER sharing -- two unrelated leaves sharing a rank
// makes a real inversion between them invisible, since the checker
// treats equal ranks as one class.
#define LOCK_RANK_RAND      250
// Leaf lock: rank above every other, so it is always legal to take and
// can be acquired from anywhere -- including while holding any other
// lock. Whatever holds it must never acquire another lock.
#define LOCK_RANK_SERIAL    255
// Same deal for the framebuffer console: taken from tty output on any
// CPU and from the panic path, never nested. A 4 MiB scroll on a raced
// cursor is a wild pointer, so the lock is not optional here.
#define LOCK_RANK_FBCON     254
// The dynamic devfs table (/dev/pts/N registration). Held only for a
// scan or an entry insert/remove, never nested, taken from open() and
// from pty teardown.
#define LOCK_RANK_DEVFS     253
// The pty pool's allocation lock. Held only to claim or release a slot;
// a slave's line-discipline lock (LOCK_RANK_TTY) is taken separately,
// never nested under this one.
#define LOCK_RANK_PTY       252

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
const char *lock_held_top_name(void);

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
