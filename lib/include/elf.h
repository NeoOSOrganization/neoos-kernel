#ifndef NEOOS_ELF_H_USER
#define NEOOS_ELF_H_USER

#include <stdint.h>

// Just enough ELF for a program to read its OWN program headers, which
// is how the C runtime finds its PT_TLS template. Names and layout are
// the standard ones, so this can be replaced wholesale by a real
// <elf.h> without touching callers.

#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_PHDR    6
#define PT_TLS     7

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

#endif
