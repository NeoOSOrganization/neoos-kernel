#ifndef NEOOS_TLB_H
#define NEOOS_TLB_H

#include <stdint.h>

#define VECTOR_IPI_TLB 0xF1

extern volatile uint64_t ipi_tlb_count;

void tlb_init(void);

// Invalidates the TLB on every CPU currently running a thread in the
// address space `pml4_phys`. Pass pml4_phys == 0 to target every CPU.
//
// MUST NOT be called while holding a lock -- see tlb.c for why.
void tlb_shootdown(uint64_t pml4_phys);

// Queues a frame to be returned to pmm only AFTER a shootdown covering
// `owner_pml4` completes. Freeing a frame while another CPU may still
// hold a stale TLB entry for it is how one process ends up writing into
// another's memory.
//
// `owner_pml4` is the address space the mapping was removed from -- the
// ONLY address space whose translations could still name this frame.
// Recording it is what lets an ordinary munmap settle with a shootdown
// targeted at its own process (usually zero IPIs) instead of an
// all-CPU broadcast. Pass 0 when the owner is not known; such a frame
// is released only by a full tlb_shootdown(0).
void tlb_defer_free(uint64_t phys, unsigned order, uint64_t owner_pml4);

// Returns queued frames to pmm: those owned by `pml4_phys`, or ALL of
// them when `pml4_phys` is 0. Called by tlb_shootdown once all
// acknowledgements are in -- never on its own, since what makes a
// deferred frame safe to release is the shootdown, not the dequeue.
void tlb_flush_deferred(uint64_t pml4_phys);

void ipi_tlb_handler(void);
void tlb_shootdown_selftest(void);

#endif
