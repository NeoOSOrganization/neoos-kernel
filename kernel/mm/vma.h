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

// Records a mapping without going through the placement policy. Used by
// the ELF loader and thread-stack allocator, which choose their own
// addresses and populate the pages themselves.
int     vma_reserve(struct process *p, uint64_t start, uint64_t len,
                    uint32_t prot, uint32_t flags);

// Populates one page for a not-present fault at `addr`. Returns 1 if a
// mapping covered it and a frame was mapped; 0 if the address belongs
// to no mapping (the caller then raises SIGSEGV) or the access is
// disallowed by the mapping's protection.
int  vma_fault(struct process *p, uint64_t addr, int write);

// Frees every mapping structure and every frame still mapped in it.
void vma_destroy_all(struct process *p);

void vma_selftest(void);
void vma_selftest_start(void);

#endif
