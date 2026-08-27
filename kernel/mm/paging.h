#ifndef NEOOS_PAGING_H
#define NEOOS_PAGING_H

#include <stdint.h>

#define PHYSMAP_BASE 0xFFFF800000000000ULL

#define PAGE_PRESENT     (1ULL << 0)
#define PAGE_WRITABLE    (1ULL << 1)
#define PAGE_USER        (1ULL << 2)
#define PAGE_NO_EXECUTE  (1ULL << 63)

// The physical-frame address field of a page-table entry: bits 12-51.
// Masking with this (rather than ~0xFFF) also strips the reserved
// high bits and PAGE_NO_EXECUTE, leaving just the frame address.
#define PAGE_ADDR_MASK   0x000FFFFFFFFFF000ULL

// Converts a physical address to its always-valid virtual alias in the
// direct physmap (see paging_init). Valid for any address within the
// first 4GiB (the physmap's coverage -- see Global Constraints).
static inline void *phys_to_virt(uint64_t phys) {
    return (void *)(uintptr_t)(PHYSMAP_BASE + phys);
}

// Inverse of phys_to_virt, for pointers that came from it (e.g. heap
// pages allocated via pmm_alloc + phys_to_virt).
static inline uint64_t virt_to_phys_physmap(uint64_t virt) {
    return virt - PHYSMAP_BASE;
}

void paging_init(void);
void paging_selftest(void);

// General-purpose 4KiB mapping API for future callers that need a
// virtual address NOT already covered by the physmap or the kernel's
// own higher-half alias -- neither of which this function should be
// used on, since both are mapped with 2MiB pages at the PD level, and
// this walks tables assuming 4KiB PT-level entries throughout.
int paging_map(uint64_t virt, uint64_t phys, uint64_t flags);
void paging_unmap(uint64_t virt);
uint64_t paging_translate(uint64_t virt); // returns the mapped physical address, or 0 if unmapped

// Allocates a fresh, zeroed page-table frame -- suitable as a new
// PML4 for a process's own address space. Caller must copy in
// whatever shared kernel entries it needs (see process.c's spawn()).
uint64_t paging_alloc_pml4(void);

// Like paging_map, but targets an arbitrary PML4 (a phys_to_virt
// pointer, not necessarily the one currently loaded in CR3) instead
// of the kernel's own live p4_table -- used by spawn() to build a new
// process's address space before it's ever loaded into CR3.
int paging_map_into(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);

// Clears one PTE in an arbitrary address space, freeing the frame when
// `free_frame` is set. Returns 1 if a mapping was removed, 0 if the
// address was not mapped.
int paging_unmap_from(uint64_t *pml4, uint64_t virt, int free_frame);

// Frees every frame belonging to the address space rooted at
// pml4_phys (user pages, page-table frames, and the PML4 itself),
// leaving the three shared kernel entries untouched. The caller must
// have switched CR3 off this address space first -- freeing the PML4
// frame hands it to the allocator, which immediately writes free-list
// links over pml4[0] (see the note in paging.c).
void free_address_space(uint64_t pml4_phys);

// Handles a write fault on a copy-on-write page (see fork()).
// Returns 1 if handled, 0 if this wasn't a recognized COW fault.
int paging_handle_cow_fault(uint64_t pml4_phys, uint64_t fault_addr);

#endif
