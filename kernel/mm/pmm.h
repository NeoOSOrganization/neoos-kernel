#ifndef NEOOS_PMM_H
#define NEOOS_PMM_H

#include <stdint.h>

#define PMM_FRAME_SIZE 4096
#define PMM_MAX_ORDER  10 // largest block = 4096 * 2^10 = 4MiB

void pmm_init(void *multiboot_info);
void pmm_selftest(void);

// Allocates 2^order contiguous frames; returns the physical base address,
// or 0 on out-of-memory. order must be <= PMM_MAX_ORDER.
uint64_t pmm_alloc(unsigned order);

// Frees a block previously returned by pmm_alloc with the same order.
// Refcount-aware: only actually returns a block to the allocator once
// every frame in it has had as many pmm_free() calls as pmm_alloc()
// (plus pmm_frame_share()) calls made against it.
void pmm_free(uint64_t phys_addr, unsigned order);

// Increments a single frame's reference count by 1. The only way a
// frame's count ever rises above 1 -- called once per shared page by
// fork()'s address-space duplication.
void pmm_frame_share(uint64_t phys);

// Returns a frame's current reference count. 1 means "sole owner";
// pmm_free() only actually returns a frame to the allocator once its
// count reaches 0.
unsigned pmm_frame_refcount(uint64_t phys);

uint64_t pmm_free_frame_count(void);
uint64_t pmm_total_frame_count(void);   // usable frames seeded at boot

#endif
