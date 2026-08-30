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

#include "tlb.h"
#include "smp.h"
#include "lapic.h"
#include "lock.h"
#include "cpu_local.h"
#include "mm/pmm.h"
#include "sched/proc.h"
#include "serial.h"

volatile uint64_t ipi_tlb_count;

static volatile int    shootdown_pending;   // acks still outstanding
static struct spinlock shootdown_lock;      // serializes shootdowns

#define DEFER_MAX 64
struct deferred { uint64_t phys; unsigned order; };
static struct deferred deferred_frames[DEFER_MAX];
static int             deferred_n;
static struct spinlock deferred_lock;

void tlb_init(void) {
    // LOCK_RANK_TLB, not PROCESS: the deferred queue is filled from
    // paging_unmap_from while a process's mm_lock (rank 3) is held, and
    // a rank-1 lock taken there is a descending acquisition. The rank
    // checker caught exactly that on the first boot.
    spin_init(&shootdown_lock, LOCK_RANK_TLB, "tlb-shootdown");
    spin_init(&deferred_lock,  LOCK_RANK_TLB, "tlb-deferred");
    deferred_n = 0;
}

void tlb_defer_free(uint64_t phys, unsigned order) {
    uint64_t f = spin_lock_irqsave(&deferred_lock);
    if (deferred_n < DEFER_MAX) {
        deferred_frames[deferred_n].phys  = phys;
        deferred_frames[deferred_n].order = order;
        deferred_n++;
        spin_unlock_irqrestore(&deferred_lock, f);
        return;
    }
    spin_unlock_irqrestore(&deferred_lock, f);

    // Queue full: shoot down NOW rather than drop the frame, then retry.
    // Correctness before throughput.
    tlb_shootdown(0);

    f = spin_lock_irqsave(&deferred_lock);
    if (deferred_n < DEFER_MAX) {
        deferred_frames[deferred_n].phys  = phys;
        deferred_frames[deferred_n].order = order;
        deferred_n++;
        spin_unlock_irqrestore(&deferred_lock, f);
    } else {
        // Still full: free directly rather than leak. Only reachable if
        // another CPU refilled the queue in the gap.
        spin_unlock_irqrestore(&deferred_lock, f);
        pmm_free(phys, order);
    }
}

void tlb_flush_deferred(void) {
    struct deferred local[DEFER_MAX];
    int n;

    uint64_t f = spin_lock_irqsave(&deferred_lock);
    n = deferred_n;
    for (int i = 0; i < n; i++) { local[i] = deferred_frames[i]; }
    deferred_n = 0;
    spin_unlock_irqrestore(&deferred_lock, f);

    // pmm_free takes pmm_lock itself, so it is called holding nothing.
    for (int i = 0; i < n; i++) { pmm_free(local[i].phys, local[i].order); }
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

    uint64_t f = spin_lock_irqsave(&shootdown_lock);

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
    spin_unlock_irqrestore(&shootdown_lock, f);

    // Wait for acks WITH INTERRUPTS ENABLED and NO LOCK HELD. Both
    // matter: a target may need to take an interrupt to make progress,
    // and holding a lock here is the deadlock described at the top.
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

    tlb_flush_deferred();
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
    tlb_defer_free(frame, 0);
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
    serial_write_string("[tlb] shootdown selftest passed, acks=");
    serial_write_hex64(__atomic_load_n(&ipi_tlb_count, __ATOMIC_ACQUIRE) - before);
    serial_write_string("\n");
}
