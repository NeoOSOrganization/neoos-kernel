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
// The NIC receive queue: the ring the network interrupt fills and the
// netrx thread drains. It is in THIS band, not up with the leaves, for
// one reason -- the thread hands it to waitq_sleep() as `release`, so
// it must rank strictly below WAITQ, exactly like the IPC guards above.
// Putting it at leaf rank looked right (it is taken from an interrupt)
// and panicked on the first sleep.
//
// Taken from the network IRQ handler, which is safe for the reason
// every IRQ-taken lock here is: locks are acquired with
// spin_lock_irqsave, so interrupts are off whenever one is held, and an
// interrupt handler therefore always starts holding nothing.
//
// A TCP connection's control block. Taken by the netrx thread
// (segments arriving), by the tcp_timer thread (retransmission,
// delayed ACK, persist, TIME_WAIT) and by syscall threads (send, recv,
// accept). It is HELD ACROSS THE TRANSMIT -- building and sending a
// segment while holding the connection whose sequence numbers it
// describes is the only way that is not a race -- which is why it must
// rank strictly below ARP, the lock the transmit goes on to take.
//
// It is NOT held under the socket lock: tcp_sock.c drops that before
// calling in, exactly as socket_sendto already drops it before
// net_udp_output, and for the same reason.
#define LOCK_RANK_TCP        13

// Held only across a ring-buffer append or removal -- never across the
// receive path, and never across the waitq wake, which is the whole
// point of handing frames to a thread.
#define LOCK_RANK_NETRX      14

// Address resolution. Taken by whichever thread is transmitting (a
// cache lookup, and the queuing of a packet behind a pending request)
// and by the netrx thread (learning from a received packet). It never
// sleeps, and it is NEVER held across a call into the driver:
// virtio_net_transmit spins waiting for the device to hand the buffer
// back, and spinning on a device with a lock held that the receive path
// also wants is a deadlock with a stack trace that explains nothing.
//
// It sits ABOVE every object lock a sender might hold on its way down
// (socket, and later TCP) and ABOVE NETRX -- which was not the first
// guess. The reasoning that put it below NETRX was "it is taken from a
// receive", which is the wrong axis: what decides a rank is what is
// ALREADY HELD when the lock is taken. The netrx thread asks
// arp_pending() while holding its own queue lock, to decide whether to
// sleep on a timeout or until the next frame, so ARP is acquired
// UNDER netrx and must rank above it. The rank checker found this on
// the first boot, which is what it is for.
//
// Nothing is ever acquired while ARP is held: every path copies what it
// needs out under the lock and transmits after dropping it.
#define LOCK_RANK_ARP        15

// The timed-sleep list. It was rank THREAD (2), which was fine while
// waitq_sleep_timeout's only caller held no lock -- but any of the IPC
// guards above hands itself to waitq_sleep_timeout as `release`, and
// timeout_add runs while that guard is still held. At rank 2 that is a
// descending acquire and an instant panic. It belongs here, directly
// under WAITQ, for the same reason WAITQ is where it is: it is taken on
// the way into a sleep, under whatever guard the sleeper was holding.
#define LOCK_RANK_TIMEOUT    16
// Per-wait-queue. Above every lock legally held across waitq_sleep() (a
// mutex passes its own guard in as `release`, carrying the mutex's
// rank), and below RUNQUEUE, since the sleep path reaches schedule()
// after the queue is updated.
// A pollable object's poll head: the list of poll/select callers
// registered on THAT object (CS5.2). Ranked below the waitq it wakes
// through, because poll_head_notify holds it across the wake -- and
// above every object lock (pipe 10, socket 12, tty 8) so a driver can
// notify with its own lock held.
#define LOCK_RANK_POLLHEAD   17
#define LOCK_RANK_WAITQ      18
#define LOCK_RANK_RUNQUEUE   19
#define LOCK_RANK_FDTABLE    20  // file descriptor table (per-bucket locks, after VFS)
#define LOCK_RANK_HEAP       21
#define LOCK_RANK_PMM        22
// The signal-queue pool: a leaf allocator taken while a process's
// p->lock (rank 1, LOCK_RANK_PROCESS) is held, and holding nothing
// itself. It sits innermost rather than beside LOCK_RANK_PROCESS
// because equal ranks are an inversion -- acquisition must be strictly
// ascending.
#define LOCK_RANK_SIGQUEUE   23
// TLB shootdown bookkeeping: the deferred-free queue is filled from
// paging_unmap_from, which runs UNDER a process's mm_lock (rank 3), so
// it must rank strictly below it. It is a leaf -- tlb_flush_deferred
// releases it before calling pmm_free.
#define LOCK_RANK_TLB        24
// Input subsystem: key event fan-out and grab. Taken from the keyboard
// IRQ (so it must be a leaf-ish rank), but held only during ring-buffer
// append -- never across tty_input_char or waitq_wake calls.
#define LOCK_RANK_INPUT     25
// The NIC's transmit path. There is ONE shared bounce buffer and one
// TX queue, and virtio_net_transmit spins waiting for the device to
// hand the buffer back -- so two concurrent transmits scribble on each
// other's frame and then race for each other's completion, and the
// loser spins ten million times before reporting a timeout it did not
// cause.
//
// D1 never noticed, because D1 transmitted from exactly one place. D2
// transmits from two: the netrx thread answering an ARP request, and
// whatever thread is sending. It is a LEAF -- taken with nothing held,
// since every ARP path drops its own lock before transmitting -- and it
// is held across the device wait, which is precisely why nothing may be
// acquired underneath it.
#define LOCK_RANK_VIRTIO_TX 26
// The kernel virtual terminals: vt_active, each VT's diff cache
// (vc->shown / shown_valid) and kd_mode. Sits ABOVE TTY because the
// write path is tty_obj_write -> t->lock -> vt_backend_output ->
// render_diff, so the VT state is touched while a tty lock is held.
// Sits BELOW FBCON because rendering calls into the con_driver
// underneath it. vt_switch and vt_scroll take t->lock then this, from
// the keyboard IRQ holding nothing else, so the order is the same on
// every path. Only the panic path takes neither, gated on the
// vt_enter_panic() latch -- see vt.c's panic-mode comment.
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
//
// There are exactly two legitimate uses, and CS5.4 adds a CI check that
// fails review on any third:
//   1. serial.c -- runs before cpu_local_init().
//   2. The panic path: fbcon_acquire() when fbcon_panicking, and vt.c
//      which skips its lock entirely when vt_panicking. The panicking
//      CPU may hold any lock, so a checked acquire would call
//      lock_panic() from inside a panic and recurse. Both latches are
//      set by the exception handler (fbcon_enter_panic / vt_enter_panic)
//      and never by the reset functions themselves -- vt_selftest calls
//      vt_panic_reset in ordinary context, and latching there would
//      silently disable VT locking for the rest of the boot.
// Everything else was converted in CS0; a registered-but-always-raw
// lock is exactly as invisible to the checker as an unregistered one.
uint64_t spin_lock_raw(struct spinlock *l);
void     spin_unlock_raw(struct spinlock *l, uint64_t flags);

// Number of spinlocks currently held by this CPU. Used by the mutex
// code to refuse to sleep with a spinlock held.
// Prints per-rank hold-time statistics gathered by a DEBUG_LOCKSTAT
// build. A no-op otherwise.
void lock_stats_dump(void);

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
