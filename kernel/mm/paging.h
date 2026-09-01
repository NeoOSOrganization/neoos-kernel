#ifndef NEOOS_PAGING_H
#define NEOOS_PAGING_H

#include <stdint.h>

#define PHYSMAP_BASE 0xFFFF800000000000ULL

#define PAGE_PRESENT     (1ULL << 0)
#define PAGE_WRITABLE    (1ULL << 1)
#define PAGE_USER        (1ULL << 2)
#define PAGE_NO_EXECUTE  (1ULL << 63)

// Bit 9 is one of the three PTE bits (9-11) the CPU ignores and leaves
// to the OS. fork() sets it on every page it makes copy-on-write, so the
// #PF handler can tell a COW page (write it -> copy/re-grant) from a
// genuinely read-only page (write it -> SIGSEGV). Before W^X every
// read-only user page was a COW page and no marker was needed.
#define PAGE_COW         (1ULL << 9)

// Bit 10: this leaf maps a frame the kernel owns permanently (a device
// BAR -- the framebuffer). free_address_space() must not pmm_free() it
// and fork() must not pmm_frame_share() it: the physical address is not
// RAM and has no place in the frame allocator.
#define PAGE_NOFREE      (1ULL << 10)

// Bit 11: this leaf still owns its frame, but the mapping is PROT_NONE.
// x86 cannot express "present and completely inaccessible" for a user
// page, so PROT_NONE is stored the way Linux stores it: PAGE_PRESENT
// cleared, the frame address kept, and this bit set so the entry is
// still recognisable as a mapped page by free_address_space(), fork()
// and a later mprotect() that makes the range accessible again. A touch
// faults not-present and reaches vma_fault(), which sees the vma's
// PROT_NONE and turns it into SIGSEGV -- without ever allocating a
// second frame over the one parked here.
#define PAGE_PROTNONE    (1ULL << 11)

// The two ways a leaf can own a frame: mapped, or parked PROT_NONE.
// Anything walking page tables to find frames must test both.
#define PAGE_HAS_FRAME   (PAGE_PRESENT | PAGE_PROTNONE)

// The physical-frame address field of a page-table entry: bits 12-51.
// Masking with this (rather than ~0xFFF) also strips the reserved
// high bits and PAGE_NO_EXECUTE, leaving just the frame address.
#define PAGE_ADDR_MASK   0x000FFFFFFFFFF000ULL

// Top of the canonical lower half. Any user pointer at or above this is
// invalid by construction, so range checks can reject it cheaply.
#define USER_ADDR_LIMIT  0x0000800000000000ULL

// Higher-half window for an explicitly mapped framebuffer (PML4[384]).
// Only used when the framebuffer physical range does not fit under the
// 4 GiB physmap -- see dev/fb.c:fb_map().
#define FB_VIRT_BASE     0xFFFFC00000000000ULL

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

// Translates through an ARBITRARY address space -- necessary to write
// into a process's memory before its PML4 has ever been loaded into
// CR3, which is what building the entry stack does. Unlike the walk
// paging_map/paging_unmap do, this handles 1GiB and 2MiB pages: the
// kernel's higher-half alias and the physmap are both large-page
// mapped, and a walk assuming 4KiB entries throughout would read a
// 2MiB PD entry as if it pointed at a page table. Returns 0 if
// unmapped.
uint64_t paging_translate_in(uint64_t pml4_phys, uint64_t virt);

// The same, through whatever is in CR3 now -- so it works for USER
// pointers, which paging_translate cannot see at all.
uint64_t paging_translate_current(uint64_t virt);

// Allocates a fresh, zeroed page-table frame -- suitable as a new
// PML4 for a process's own address space. Caller must copy in
// whatever shared kernel entries it needs (see process.c's spawn()).
uint64_t paging_alloc_pml4(void);

// Like paging_map, but targets an arbitrary PML4 (a phys_to_virt
// pointer, not necessarily the one currently loaded in CR3) instead
// of the kernel's own live p4_table -- used by spawn() to build a new
// process's address space before it's ever loaded into CR3.
int paging_map_into(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags);

// Map [phys, phys+len) at [virt, virt+len) in the kernel PML4, 4 KiB at
// a time. len is rounded up to a page. Returns 0, or <0 on the first
// allocation failure (partial mapping left in place -- caller's problem).
int paging_map_range(uint64_t virt, uint64_t phys, uint64_t len, uint64_t flags);

// Clears one PTE in an arbitrary address space, freeing the frame when
// `free_frame` is set. Returns 1 if a mapping was removed, 0 if the
// address was not mapped.
int paging_unmap_from(uint64_t *pml4, uint64_t virt, int free_frame);

// Rewrites the ACCESS bits of one leaf entry in an arbitrary address
// space, keeping the frame and everything in it. `flags` is a leaf flag
// set of the same shape paging_map_into takes (PAGE_USER, optionally
// PAGE_WRITABLE / PAGE_NO_EXECUTE) plus PAGE_PRESENT when the page is
// to stay accessible; pass 0 for PROT_NONE, which parks the entry as
// PAGE_PROTNONE. Returns 1 if an entry was rewritten, 0 if the address
// owns no frame.
//
// This is what mprotect() uses. Unmapping the range instead -- what it
// used to do -- threw the page contents away, which mprotect must never
// do for a range it only makes less permissive.
int paging_setprot_from(uint64_t *pml4, uint64_t virt, uint64_t flags);

// 1 if every page of [addr, addr+len) is present, user-accessible and
// safe for the KERNEL to write in the current address space. Copy-on-
// write pages are broken here rather than rejected: writing them is
// legal, but a CPL0 write would fault with user=0 and never reach the
// COW handler. Signal delivery uses this so a bad user stack becomes a
// clean kill instead of a kernel fault.
int user_range_writable(uint64_t addr, uint64_t len);
int user_range_readable(uint64_t addr, uint64_t len);

// Kernel W^X (see paging.c). Called once on the BSP after heap_init,
// before AP bring-up.
int      paging_split_huge(uint64_t virt);      // 2MiB leaf -> 512 4KiB; 0 ok, <0 oom
void     paging_protect_kernel(void);           // .text RO+X, .rodata RO+NX, rest NX
uint64_t paging_leaf_entry(uint64_t virt);      // leaf PTE/PDE for virt, or 0
void     wxorx_selftest(void);

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
