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

// Queues a frame to be returned to pmm only AFTER the next shootdown
// completes. Freeing a frame while another CPU may still hold a stale
// TLB entry for it is how one process ends up writing into another's
// memory.
void tlb_defer_free(uint64_t phys, unsigned order);

// Returns every queued frame to pmm. Called by tlb_shootdown once all
// acknowledgements are in.
void tlb_flush_deferred(void);

void ipi_tlb_handler(void);
void tlb_shootdown_selftest(void);

#endif
