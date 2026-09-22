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
#include "mm/paging.h"
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
// It has to be UNBOUNDED, and it must NEVER ALLOCATE. Tearing down an
// address space defers every user page and every page-table frame at
// once, and vma_munmap does its unmapping UNDER the process's mm_lock,
// where a shootdown is forbidden (a target spinning on that mm_lock
// with interrupts off can never acknowledge). The queue used to be a
// 256-entry array with a kmalloc'd linked-list overflow -- and under
// memory pressure that kmalloc failed, so the frame was LEAKED for
// good. That is a death spiral: the backlog that exhausted memory is
// itself memory the next shootdown would have returned, and leaking it
// makes the next failure likelier ("[tlb] out of memory deferring a
// frame; leaking it" by the hundred, next to a GL client under wm).
//
// So the bookkeeping is PER-FRAME METADATA sized once at boot: one
// slot per frame pmm can ever hand out, linked into a single queue.
// Deferring a frame just links its slot, so it cannot fail. The slot
// cannot live inside the deferred frame itself: another CPU may still
// write that frame through a stale TLB entry (or set A/D bits in a
// freed page-table frame) until the shootdown completes.
//
// Each slot records the address space the frame was unmapped from. A
// stale translation can only exist on a CPU running in that address
// space, so a shootdown aimed at one pml4 may release exactly its own
// frames -- which is what keeps an ordinary munmap from broadcasting an
// IPI to every CPU. Owner 0 means "not known": released only by a full
// shootdown. A COW-shared frame can be deferred again while already
// queued (two sharers unmapping it); the slot then counts both
// references, and if the owners differ it degrades to owner 0, since
// only a full shootdown covers both address spaces.
#define DEFER_BACKLOG 256   // queued frames that count as a backlog

struct defer_slot {
    uint32_t next;     // frame index + 1 of the next queued slot; 0 ends
    uint32_t owner;    // owner pml4's frame index + 1; 0 = unknown
    uint32_t count;    // deferred references this slot holds
    uint8_t  order;
    uint8_t  queued;
    uint16_t pad;
};
#define SLOTS_PER_PAGE (PMM_FRAME_SIZE / sizeof(struct defer_slot))
#define SLOT_DIR_MAX   ((4ULL * 1024 * 1024 * 1024 / PMM_FRAME_SIZE) / SLOTS_PER_PAGE)
static struct defer_slot *slot_dir[SLOT_DIR_MAX];
static uint64_t slot_limit;         // frames covered by slot_dir
static uint32_t deferred_head;      // frame index + 1; 0 = empty
static int      deferred_n;         // slots queued
static struct spinlock deferred_lock;

static struct defer_slot *slot_of(uint64_t frame) {
    return &slot_dir[frame / SLOTS_PER_PAGE][frame % SLOTS_PER_PAGE];
}

static uint32_t owner_key(uint64_t pml4_phys) {
    return pml4_phys ? (uint32_t)(pml4_phys / PMM_FRAME_SIZE) + 1 : 0;
}

void tlb_init(void) {
    // LOCK_RANK_TLB, not PROCESS: the deferred queue is filled from
    // paging_unmap_from while a process's mm_lock (rank 3) is held, and
    // a rank-1 lock taken there is a descending acquisition. The rank
    // checker caught exactly that on the first boot.
    spin_init(&deferred_lock,  LOCK_RANK_TLB, "tlb-deferred");
    deferred_n = 0;
    deferred_head = 0;

    // One slot per frame pmm can ever hand out, taken now, while memory
    // is plentiful, so that deferring a frame later never allocates.
    // 16 bytes per 4KiB frame: 0.4% of RAM.
    slot_limit = pmm_frame_limit();
    uint64_t pages = (slot_limit + SLOTS_PER_PAGE - 1) / SLOTS_PER_PAGE;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc(0);
        if (!phys) { lock_panic("tlb_init: no memory for the deferred-free slots", "tlb", 0); }
        uint64_t *w = (uint64_t *)phys_to_virt(phys);
        for (unsigned j = 0; j < PMM_FRAME_SIZE / 8; j++) { w[j] = 0; }
        slot_dir[i] = (struct defer_slot *)w;
    }
}

void tlb_defer_free(uint64_t phys, unsigned order, uint64_t owner_pml4) {
    uint64_t frame = phys / PMM_FRAME_SIZE;
    if (frame >= slot_limit) {
        // pmm never hands out a frame at or past its limit, so this is
        // a caller passing something pmm did not allocate. Returning it
        // to pmm would be worse than dropping it.
        lock_panic("tlb_defer_free: frame outside pmm's range", "tlb", 0);
    }
    uint32_t own = owner_key(owner_pml4);

    uint64_t f = spin_lock_irqsave(&deferred_lock);
    struct defer_slot *sl = slot_of(frame);
    if (sl->queued) {
        if (sl->owner != own) { sl->owner = 0; }
        sl->count++;
    } else {
        sl->queued = 1;
        sl->count  = 1;
        sl->owner  = own;
        sl->order  = (uint8_t)order;
        sl->next   = deferred_head;
        deferred_head = (uint32_t)frame + 1;
        deferred_n++;
    }
    spin_unlock_irqrestore(&deferred_lock, f);
}

void tlb_flush_deferred(uint64_t pml4_phys) {
    // pmm_free takes pmm_lock, which may not be taken beneath
    // deferred_lock (the innermost rank), so releasable slots are
    // unlinked under the lock in small batches, copied onto the stack,
    // and freed with the lock dropped. Unlinking clears the slot, so a
    // concurrent re-defer of the same frame starts a fresh entry rather
    // than riding on one about to be released.
    uint32_t want = owner_key(pml4_phys);
    for (;;) {
        struct { uint64_t phys; uint32_t count; uint8_t order; } batch[32];
        int nb = 0;

        uint64_t lf = spin_lock_irqsave(&deferred_lock);
        uint32_t *link = &deferred_head;
        while (*link && nb < 32) {
            uint64_t fr = *link - 1;
            struct defer_slot *sl = slot_of(fr);
            if (pml4_phys == 0 || sl->owner == want) {
                batch[nb].phys  = fr * PMM_FRAME_SIZE;
                batch[nb].count = sl->count;
                batch[nb].order = sl->order;
                nb++;
                *link = sl->next;
                sl->next = 0; sl->queued = 0; sl->count = 0; sl->owner = 0;
                deferred_n--;
            } else {
                link = &sl->next;          // someone else's; leave it queued
            }
        }
        spin_unlock_irqrestore(&deferred_lock, lf);

        for (int i = 0; i < nb; i++) {
            for (uint32_t c = 0; c < batch[i].count; c++) { pmm_free(batch[i].phys, batch[i].order); }
        }
        if (nb < 32) { break; }
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

// True when the deferred queue has built up enough frames that only a
// full shootdown should be trusted to release them (orphans included). Used by callers that are in a safe
// context to drain it (no lock, interrupts enable-able) but would not
// otherwise issue a shootdown: a process exiting on a lightly-loaded
// system, the idle loop.
int tlb_deferred_backlog(void) {
    uint64_t lf = spin_lock_irqsave(&deferred_lock);
    int backlog = deferred_n >= DEFER_BACKLOG;
    spin_unlock_irqrestore(&deferred_lock, lf);
    return backlog;
}

// Drain the deferred queue via a full shootdown IF a backlog has built
// up. Safe to call from any context with no lock held and interrupts
// enable-able. A no-op when there is nothing stuck.
void tlb_drain_if_backlogged(void) {
    if (tlb_deferred_backlog()) { tlb_shootdown(0); }
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

    // Orphan backlog. The deferred queue is normally drained per-owner:
    // each address space's frames are released by a tlb_shootdown aimed
    // at its own pml4 -- a live munmap/mprotect's vma_tlb_settle, or
    // proc_reap for the whole address space of an exited process. But
    // proc_reap only runs when a zombie is wait()ed. A process that
    // exits and is reparented to an init that never reaps it -- the
    // boot-time network self-tests are exactly this -- leaves its ENTIRE
    // address space, thousands of frames, queued with an owner no future
    // shootdown will ever name. Those pile up in the queue, and a
    // sustained munmap workload
    // then bleeds pmm dry; past that point vma_fault cannot get a frame
    // and the next user write faults on a VMA-covered-but-unmapped page.
    //
    // A full shootdown (pml4_phys == 0) reaches every CPU and releases
    // EVERY owner's frames, so promoting to one whenever a backlog has
    // built up clears the orphans. Self-limiting: the first promoted
    // shootdown empties the backlog, so the next caller is not promoted.
    if (pml4_phys != 0) {
        if (tlb_deferred_backlog()) { pml4_phys = 0; }
    }

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
    // A COW-shared frame deferred by two DIFFERENT address spaces: the
    // slot must hold both references, and since no single targeted
    // shootdown covers both owners, neither may release it -- only a
    // full one, which must then return BOTH references.
    //
    // Asserted on the frame's own refcount, not the global free count:
    // the network kernel threads are already allocating by now. The
    // test keeps a third reference of its own throughout, so the frame
    // can never be freed and handed to someone else mid-test and every
    // expected count is exact.
    uint64_t shared = pmm_alloc(0);
    if (!shared) { serial_write_string("[tlb] selftest FAILED: no memory (shared)\n"); return; }
    pmm_frame_share(shared);
    pmm_frame_share(shared);                 // refcount 3: two sharers + the test
    tlb_defer_free(shared, 0, mine);
    tlb_defer_free(shared, 0, theirs);
    tlb_shootdown(mine);
    tlb_shootdown(theirs);
    if (pmm_frame_refcount(shared) != 3) {
        serial_write_string("[tlb] selftest FAILED: two-owner frame released by a targeted shootdown\n");
        return;
    }
    tlb_shootdown(0);
    if (pmm_frame_refcount(shared) != 1) {
        serial_write_string("[tlb] selftest FAILED: full shootdown did not release both references\n");
        return;
    }
    pmm_free(shared, 0);

    serial_write_string("[tlb] shootdown selftest passed, acks=");
    serial_write_hex64(__atomic_load_n(&ipi_tlb_count, __ATOMIC_ACQUIRE) - before);
    serial_write_string("\n");
}
