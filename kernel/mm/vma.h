#ifndef NEOOS_VMA_H
#define NEOOS_VMA_H

#include <stdint.h>

#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

// Internal vma->flags bit (above the MAP_* range): the mapping is backed
// by fixed physical frames the kernel owns (a device, e.g. the
// framebuffer). Its pages are mapped eagerly at creation and must NEVER
// be pmm_free()d on unmap or teardown.
#define VMA_PHYS      0x40

// The gap between the ELF image (0x200000000000, see userland/user.ld)
// and the thread stacks (0x700000000000). Grows up.
#define MMAP_BASE  0x0000500000000000ULL
#define MMAP_LIMIT 0x0000600000000000ULL

struct vma {
    uint64_t    start, end;   // [start, end), page-aligned
    uint32_t    prot;
    uint32_t    flags;
    struct vma *next;         // list is sorted by start, non-overlapping
};

struct process;

int64_t vma_mmap(struct process *p, uint64_t addr, uint64_t len,
                 uint32_t prot, uint32_t flags);
int     vma_munmap(struct process *p, uint64_t addr, uint64_t len);
int     vma_mprotect(struct process *p, uint64_t addr, uint64_t len, uint32_t prot);

// Maps [phys, phys+len) into `p` at a kernel-chosen address, eagerly
// (every page mapped now), backed by device-owned physical frames.
// prot must not include PROT_EXEC (W^X). Returns the user address, or a
// negative errno.
int64_t vma_map_phys(struct process *p, uint64_t phys, uint64_t len, uint32_t prot);

// Populates one page for a not-present fault at `addr`. Returns 1 if a
// mapping covered it and a frame was mapped; 0 if the address belongs
// to no mapping (the caller then raises SIGSEGV) or the access is
// disallowed by the mapping's protection.
int  vma_fault(struct process *p, uint64_t addr, int write);

// Frees every mapping structure and every frame still mapped in it.
void vma_destroy_all(struct process *p);

// Gives `dst` a copy of `src`'s mapping list and its mmap cursor. This
// is fork's other half: duplicating the page tables hands the child
// every page the parent had already touched, and the vma list is what
// says which addresses are legal at all.
//
// Returns 1, or 0 on out-of-memory having freed whatever it had already
// copied -- `dst` is left with an empty list either way, never a
// partial one.
int vma_copy_all(struct process *dst, struct process *src);

// Frees the mapping STRUCTURES and nothing else -- no unmapping, no
// frames returned. For a caller that is discarding the whole address
// space by another route (fork's failure paths, which hand the page
// tables to free_address_space); calling vma_destroy_all there instead
// would free every frame a second time.
void vma_forget_all(struct process *p);

void vma_selftest(void);
void vma_selftest_start(void);

#endif
