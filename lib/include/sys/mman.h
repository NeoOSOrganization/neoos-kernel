#ifndef NEOOS_SYS_MMAN_H
#define NEOOS_SYS_MMAN_H

#include <stdint.h>

// Linux's values, so a program compiled against Linux headers passes
// the right bits. Only the subset NeoOS implements is defined: a
// constant for a flag the kernel would reject is a promise it cannot
// keep.
#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20

#define MAP_FAILED ((void *)-1)

// POSIX shapes. `fd` must be -1 and `flags` must include MAP_ANONYMOUS:
// file-backed mappings are not implemented and return MAP_FAILED with
// -ENOSYS from the kernel. See docs/stdlib.md.
void *mmap(void *addr, uint64_t length, int prot, int flags, int fd, int64_t offset);
int   munmap(void *addr, uint64_t length);
int   mprotect(void *addr, uint64_t length, int prot);

// The raw forms, returning the kernel's own negative errno rather than
// MAP_FAILED. Kept because the rest of this library returns errors that
// way, and because the C runtime uses mmap before there is anywhere to
// report an error to.
long mmap_raw(unsigned long addr, unsigned long len, int prot, int flags);
int  munmap_raw(unsigned long addr, unsigned long len);

#endif
