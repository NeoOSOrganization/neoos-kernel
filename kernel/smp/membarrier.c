// kernel/smp/membarrier.c -- the cross-CPU half of membarrier(2).
//
// Mirrors kernel/smp/tlb.c's shootdown pattern almost exactly (same
// deadlock constraint, same "wait with interrupts enabled and no lock
// held" requirement, same busy-guard instead of a rank-checked
// spinlock), but simpler: there is no deferred-free queue, because a
// memory barrier has nothing to release afterward. See tlb.c's own
// file comment for the deadlock reasoning this borrows.

#include "smp/membarrier.h"
#include "smp/smp.h"
#include "drivers/irq/lapic.h"
#include "sync/lock.h"
#include "arch/cpu_local.h"
#include "drivers/char/serial.h"

static volatile int membarrier_pending;   // acks still outstanding
static volatile int membarrier_busy;      // one broadcast in flight at a time

static void membarrier_acquire(void) {
    while (__atomic_exchange_n(&membarrier_busy, 1, __ATOMIC_ACQUIRE)) {
        __asm__ volatile ("pause");
    }
}

static void membarrier_release(void) {
    __atomic_store_n(&membarrier_busy, 0, __ATOMIC_RELEASE);
}

void ipi_membarrier_handler(void) {
    // Nothing to do: entering this handler is itself the serializing
    // event membarrier(2) promises the caller (see membarrier.h). The
    // ack is the whole point of the interrupt.
    __atomic_fetch_sub(&membarrier_pending, 1, __ATOMIC_ACQ_REL);
    lapic_send_eoi();
}

void membarrier_global(void) {
    if (lock_held_depth() != 0) {
        lock_panic("membarrier_global with a lock held", "membarrier", 0);
    }

    uint64_t caller_flags;
    __asm__ volatile ("pushfq; pop %0" : "=r"(caller_flags) :: "memory");

    membarrier_acquire();

    int self   = (int)(this_cpu() - &cpus[0]);
    int online = smp_online_count();

    int targets[MAX_CPUS];
    int ntargets = 0;
    for (int i = 0; i < online; i++) {
        if (i == self) { continue; }
        targets[ntargets++] = i;
    }
    __atomic_store_n(&membarrier_pending, ntargets, __ATOMIC_RELEASE);

    // This CPU's own instruction stream is already ordered by the
    // syscall entry/exit it is sitting inside; only the OTHER CPUs
    // need the IPI.
    for (int i = 0; i < ntargets; i++) {
        lapic_send_ipi(smp_lapic_for_index(targets[i]), VECTOR_IPI_MEMBARRIER);
    }

    __asm__ volatile ("sti");
    int spins = 0;
    while (__atomic_load_n(&membarrier_pending, __ATOMIC_ACQUIRE) > 0) {
        __asm__ volatile ("pause");
        if (++spins > 50000000) {
            serial_write_string("[membarrier] broadcast timed out; continuing\n");
            __atomic_store_n(&membarrier_pending, 0, __ATOMIC_RELEASE);
            break;
        }
    }

    if (!(caller_flags & (1ULL << 9))) { __asm__ volatile ("cli"); }

    membarrier_release();
}
