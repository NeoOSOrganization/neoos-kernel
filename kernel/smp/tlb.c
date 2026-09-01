// kernel/tlb.c -- TLB shootdown.
//
// Two CPUs running threads of one process share a page table, so an
// unmap on one leaves a stale TLB entry on the other.
//
// THE DEADLOCK THIS DESIGN AVOIDS: spin_lock_irqsave clears IF, so a CPU
// spinning on a lock cannot take an IPI. If the sender held mm_lock
// while waiting for acknowledgements, and the target were spinning on
// that same mm_lock, neither would ever move.
//
// So the sequence is fixed:
//   1. modify the page table UNDER mm_lock
//   2. record the freed frames
//   3. RELEASE mm_lock
//   4. send the IPI and wait for acks WITH IF ENABLED
//
// and the rule that falls out of it: a frame goes back to pmm only after
// every ack is in. A stale TLB entry pointing at a page the address
// space still owns is harmless; one pointing at a page pmm has already
// re-handed to another process is memory corruption.

#include "smp/tlb.h"
#include "smp/smp.h"
#include "drivers/irq/lapic.h"
#include "sync/lock.h"
#include "arch/cpu_local.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "sched/proc.h"
#include "drivers/char/serial.h"

volatile uint64_t ipi_tlb_count;

static volatile int    shootdown_pending;   // acks still outstanding

// Serialises shootdowns, and is NOT a rank-checked spinlock on purpose.
//
// shootdown_pending is a single counter, so only one shootdown may be
// in flight at a time -- but the wait for acknowledgements runs with
// interrupts ENABLED and no lock held, which means the waiting thread
// can be preempted. Holding a real spinlock across that would hit
// schedule()'s "no spinlock held" assertion; holding one with
// interrupts off would reintroduce the deadlock the design note above
// describes.
//
// So it is a plain test-and-set. A CPU that is preempted while holding
// it delays other CPUs' shootdowns until it runs again, which is a
// latency cost rather than a correctness one. The alternative -- what
// this replaced -- was releasing the lock before the wait, which let
// two shootdowns share one counter: each decremented the other's acks,
// one returned early and the other timed out. "[tlb] shootdown timed
// out; continuing" in a log with two concurrent munmaps was exactly
// that, not a lost IPI.
static volatile int shootdown_busy;

static void shootdown_acquire(void) {
    while (__atomic_exchange_n(&shootdown_busy, 1, __ATOMIC_ACQUIRE)) {
        __asm__ volatile ("pause");
    }
}

static void shootdown_release(void) {
    __atomic_store_n(&shootdown_busy, 0, __ATOMIC_RELEASE);
}

// The deferred-free queue: frames whose mappings are gone but which
// must not go back to pmm until every CPU has acknowledged a shootdown.
//
// It has to be UNBOUNDED, and that is not a nicety. Tearing down an
// address space defers every user page and every page-table frame at
// once -- hundreds of them -- and vma_munmap does its unmapping UNDER
// the process's mm_lock. The previous fixed 64-entry array handled a
// full queue by performing an emergency shootdown on the spot, which
// meant calling tlb_shootdown with mm_lock held: exactly what
// tlb_shootdown asserts against, because a target spinning on that
// same mm_lock with interrupts off can never acknowledge. It fired as
// "[lock] PANIC: tlb_shootdown with a lock held" once enough processes
// were exiting concurrently to fill the array.
//
// So the array is a fast path and a linked list is the overflow. The
// node is allocated BEFORE deferred_lock is taken -- kmalloc's heap
// lock ranks above mm_lock but below nothing this path holds, whereas
// deferred_lock is the innermost rank in the kernel and nothing may be
// acquired beneath it.
//
// Each entry also records the address space it was unmapped from. A
// stale translation for a frame can only exist on a CPU running in that
// address space, so a shootdown aimed at one pml4 may release exactly
// its own frames -- which is what keeps an ordinary munmap from having
// to broadcast an IPI to every CPU in the machine. Owner 0 means "not
// known"; those wait for a full shootdown.
#define DEFER_MAX 256
struct deferred { uint64_t phys; uint64_t owner; unsigned order; };
static struct deferred deferred_frames[DEFER_MAX];
static int             deferred_n;

struct deferred_node {
    struct deferred_node *next;
    uint64_t phys;
    uint64_t owner;
    unsigned order;
};
static struct deferred_node *deferred_overflow;
static struct spinlock deferred_lock;

void tlb_init(void) {
    // LOCK_RANK_TLB, not PROCESS: the deferred queue is filled from
    // paging_unmap_from while a process's mm_lock (rank 3) is held, and
    // a rank-1 lock taken there is a descending acquisition. The rank
    // checker caught exactly that on the first boot.
    spin_init(&deferred_lock,  LOCK_RANK_TLB, "tlb-deferred");
    deferred_n = 0;
}

void tlb_defer_free(uint64_t phys, unsigned order, uint64_t owner_pml4) {
    uint64_t f = spin_lock_irqsave(&deferred_lock);
    if (deferred_n < DEFER_MAX) {
        deferred_frames[deferred_n].phys  = phys;
        deferred_frames[deferred_n].owner = owner_pml4;
        deferred_frames[deferred_n].order = order;
        deferred_n++;
        spin_unlock_irqrestore(&deferred_lock, f);
        return;
    }
    spin_unlock_irqrestore(&deferred_lock, f);

    // Overflow. The node is allocated with deferred_lock NOT held: it
    // is the innermost rank in the kernel, so kmalloc cannot be called
    // beneath it.
    struct deferred_node *n = (struct deferred_node *)kmalloc(sizeof(*n));
    if (!n) {
        // Out of memory with a frame that must not be reused yet. LEAK
        // it, deliberately. Handing it back to pmm now is the exact
        // memory-corruption this whole mechanism exists to prevent -- a
        // stale TLB entry pointing at a page another process has since
        // been given -- and a leaked frame under memory pressure is a
        // far better outcome than that.
        serial_write_string("[tlb] out of memory deferring a frame; leaking it\n");
        return;
    }
    n->phys  = phys;
    n->owner = owner_pml4;
    n->order = order;

    f = spin_lock_irqsave(&deferred_lock);
    n->next = deferred_overflow;
    deferred_overflow = n;
    spin_unlock_irqrestore(&deferred_lock, f);
}

void tlb_flush_deferred(uint64_t pml4_phys) {
    // The array is drained one entry at a time rather than copied out
    // wholesale. A 256-entry copy is 4KiB, which is too much to put on
    // a 16KiB kernel stack that interrupts also nest on -- and making
    // the copy `static` instead is worse, because two CPUs draining at
    // once would clobber each other's copy the moment the first one
    // dropped the lock. Taking the lock per entry costs nothing next to
    // the pmm_free it guards.
    //
    // Scanned from the back with a swap-remove, so releasing a subset
    // stays O(n) and the surviving entries need no shuffling. `i` is
    // re-read from deferred_n on every pass because the lock is dropped
    // across pmm_free and another CPU may have pushed or pulled.
    for (int i = 0; ; i++) {
        uint64_t lf = spin_lock_irqsave(&deferred_lock);
        if (i >= deferred_n) { spin_unlock_irqrestore(&deferred_lock, lf); break; }
        struct deferred d = deferred_frames[i];
        if (pml4_phys != 0 && d.owner != pml4_phys) {
            spin_unlock_irqrestore(&deferred_lock, lf);
            continue;                      // someone else's; leave it queued
        }
        deferred_frames[i] = deferred_frames[--deferred_n];
        spin_unlock_irqrestore(&deferred_lock, lf);
        pmm_free(d.phys, d.order);
        i--;                               // re-examine the entry swapped in
    }

    // The overflow list is detached with a single pointer swap, so it
    // needs no copy at all and can be walked outside the lock. Entries
    // that do not belong to this shootdown go back on afterwards.
    uint64_t f = spin_lock_irqsave(&deferred_lock);
    struct deferred_node *over = deferred_overflow;
    deferred_overflow = 0;
    spin_unlock_irqrestore(&deferred_lock, f);

    struct deferred_node *keep = 0;
    while (over) {
        struct deferred_node *next = over->next;
        if (pml4_phys != 0 && over->owner != pml4_phys) {
            over->next = keep;
            keep = over;
        } else {
            pmm_free(over->phys, over->order);
            kfree(over);
        }
        over = next;
    }
    if (keep) {
        struct deferred_node *tail = keep;
        while (tail->next) { tail = tail->next; }
        f = spin_lock_irqsave(&deferred_lock);
        tail->next = deferred_overflow;
        deferred_overflow = keep;
        spin_unlock_irqrestore(&deferred_lock, f);
    }
}

void ipi_tlb_handler(void) {
    // A whole-CR3 reload rather than per-page invlpg: the ranges here
    // are small, but reloading CR3 is one instruction and cannot miss an
    // entry. Precision can come later if it ever measures.
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");

    __atomic_fetch_add(&ipi_tlb_count, 1, __ATOMIC_ACQ_REL);
    __atomic_fetch_sub(&shootdown_pending, 1, __ATOMIC_ACQ_REL);
    lapic_send_eoi();
}

void tlb_shootdown(uint64_t pml4_phys) {
    if (lock_held_depth() != 0) {
        lock_panic("tlb_shootdown with a lock held", "tlb", 0);
    }

    // The ack wait below MUST run with interrupts enabled (see the
    // deadlock note at the top of this file), but that is this
    // function's business, not the caller's. Leaving IF set on the way
    // out silently hands preemption to a caller that had deliberately
    // disabled it -- which is how kmain, whose only shootdown is the
    // selftest, ended up being scheduled away mid-boot and never
    // printing the "starting scheduler" marker.
    uint64_t caller_flags;
    __asm__ volatile ("pushfq; pop %0" : "=r"(caller_flags) :: "memory");

    shootdown_acquire();

    int self   = (int)(this_cpu() - &cpus[0]);
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
    __atomic_store_n(&shootdown_pending, ntargets, __ATOMIC_RELEASE);

    // Local invalidation first: this CPU needs no IPI.
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory");

    for (int i = 0; i < ntargets; i++) {
        lapic_send_ipi(smp_lapic_for_index(targets[i]), VECTOR_IPI_TLB);
    }

    // Wait for acks WITH INTERRUPTS ENABLED and NO SPINLOCK HELD. Both
    // matter: a target may need to take an interrupt to make progress,
    // and holding a lock here is the deadlock described at the top.
    // shootdown_busy is still held, which is why it is not a spinlock.
    __asm__ volatile ("sti");
    int spins = 0;
    while (__atomic_load_n(&shootdown_pending, __ATOMIC_ACQUIRE) > 0) {
        __asm__ volatile ("pause");
        if (++spins > 50000000) {
            serial_write_string("[tlb] shootdown timed out; continuing\n");
            __atomic_store_n(&shootdown_pending, 0, __ATOMIC_RELEASE);
            break;
        }
    }

    if (!(caller_flags & (1ULL << 9))) { __asm__ volatile ("cli"); }

    shootdown_release();

    // Release exactly what this shootdown covered. The deferred list is
    // global -- one address space's frames sit in it next to another's
    // -- so a targeted shootdown may only release frames tagged with
    // the pml4 it targeted; handing back a neighbour's would put a frame
    // in pmm while a CPU this shootdown never touched still had a
    // translation for it. A full shootdown (pml4_phys == 0) reached
    // every CPU and releases everything.
    tlb_flush_deferred(pml4_phys);
}

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
    tlb_defer_free(frame, 0, 0);   // owner 0: released only by a full shootdown
    if (pmm_free_frame_count() != free_before) {
        serial_write_string("[tlb] selftest FAILED: deferred free returned the frame early\n");
        return;
    }

    // And the IPI itself must reach the other CPUs.
    uint64_t before = __atomic_load_n(&ipi_tlb_count, __ATOMIC_ACQUIRE);
    tlb_shootdown(0);   // all address spaces; also flushes the deferred queue

    if (pmm_free_frame_count() != free_before + 1) {
        serial_write_string("[tlb] selftest FAILED: deferred free never returned the frame\n");
        return;
    }
    if (smp_online_count() > 1 &&
        __atomic_load_n(&ipi_tlb_count, __ATOMIC_ACQUIRE) <= before) {
        serial_write_string("[tlb] selftest FAILED: shootdown IPI never delivered\n");
        return;
    }

    // Ownership tagging: a shootdown aimed at one address space must
    // release that address space's deferred frames and NOBODY else's.
    // Getting this wrong is silent -- a frame handed back to pmm while
    // another CPU still has a translation for it -- so it is asserted
    // rather than assumed. The two owner values are fake pml4
    // addresses: no CPU is running in either, so neither shootdown
    // sends an IPI, which is exactly the case the fast path relies on.
    uint64_t owned = pmm_alloc(0);
    if (!owned) { serial_write_string("[tlb] selftest FAILED: no memory (owner)\n"); return; }
    uint64_t mine = 0xAAAA000, theirs = 0xBBBB000;
    free_before = pmm_free_frame_count();
    tlb_defer_free(owned, 0, mine);

    tlb_shootdown(theirs);
    if (pmm_free_frame_count() != free_before) {
        serial_write_string("[tlb] selftest FAILED: shootdown released another address space's frame\n");
        return;
    }
    tlb_shootdown(mine);
    if (pmm_free_frame_count() != free_before + 1) {
        serial_write_string("[tlb] selftest FAILED: targeted shootdown did not release its own frame\n");
        return;
    }
    serial_write_string("[tlb] shootdown selftest passed, acks=");
    serial_write_hex64(__atomic_load_n(&ipi_tlb_count, __ATOMIC_ACQUIRE) - before);
    serial_write_string("\n");
}
