#ifndef NEOOS_MMIO_H
#define NEOOS_MMIO_H

#include <stdint.h>

// Uncached mappings for memory-mapped device registers (PCI memory
// BARs). The physmap maps everything write-back, which is only safe for
// MMIO while firmware MTRRs mark the PCI hole uncacheable -- true under
// SeaBIOS, not something a driver should rely on.
//
// The window sits INSIDE PML4[256], the physmap's slot: that entry and
// PML4[511] are the only kernel entries process address spaces copy
// (sched/proc.c), so a mapping anywhere else would vanish the first time
// a driver ran on a process's CR3. (FB_VIRT_BASE, in PML4[384], has
// exactly that latent problem; it is only a fallback nobody takes.)
#define MMIO_VIRT_BASE (0xFFFF800000000000ULL + (256ULL << 30))
#define MMIO_VIRT_SIZE (1ULL << 30)

void mmio_init(void);
// Maps [phys, phys+len) uncached (PCD|PWT: PAT entry 3, UC) and returns
// the virtual address of `phys`, or 0 when the window is exhausted or a
// page table cannot be allocated. Mappings are permanent.
volatile void *mmio_map(uint64_t phys, uint64_t len);
void mmio_selftest(void);

#endif
