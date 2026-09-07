#ifndef NEOOS_MEMBARRIER_H
#define NEOOS_MEMBARRIER_H

#include <stdint.h>

#define VECTOR_IPI_MEMBARRIER 0xF3

// Linux's membarrier(2) command bits (x86_64, from linux/membarrier.h).
// Only these are recognised -- see membarrier.c for which of them this
// implementation actually distinguishes.
#define MEMBARRIER_CMD_QUERY                              0
#define MEMBARRIER_CMD_GLOBAL                              (1 << 0)
#define MEMBARRIER_CMD_GLOBAL_EXPEDITED                     (1 << 1)
#define MEMBARRIER_CMD_REGISTER_GLOBAL_EXPEDITED            (1 << 2)
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED                    (1 << 3)
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED           (1 << 4)
#define MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE          (1 << 5)
#define MEMBARRIER_CMD_REGISTER_PRIVATE_EXPEDITED_SYNC_CORE (1 << 6)

// membarrier(cmd, flags) -- see kernel/syscall/sys_proc.c's sys_membarrier
// for the syscall entry point itself. This is the mechanism it calls
// for any command that must actually order memory across CPUs.
//
// Sends VECTOR_IPI_MEMBARRIER to every online CPU but this one and
// waits for each to acknowledge before returning. Nothing in the
// handler needs to DO anything beyond that: on x86-64, taking an
// interrupt is itself a serializing event (the CPU cannot speculate
// past one), so IPI delivery IS the barrier Linux's membarrier(2)
// promises -- the same property tlb_shootdown already leans on for
// TLB consistency, just without any page-table work on the far side.
//
// MUST NOT be called while holding a lock, for the same deadlock
// reason as tlb_shootdown (see kernel/smp/tlb.c's file comment): the
// wait for acknowledgements runs with interrupts enabled and no lock
// held, so a target that needs an interrupt to make progress still can.
void membarrier_global(void);

void ipi_membarrier_handler(void);

#endif
