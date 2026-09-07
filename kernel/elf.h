#ifndef NEOOS_ELF_H
#define NEOOS_ELF_H

#include <stdint.h>

// What the loader learned about an image, beyond where to start it.
//
// Everything here exists to be handed to userland in the auxiliary
// vector: the C runtime cannot find its own program headers (AT_PHDR)
// or its TLS template without being told, because a static executable
// has no dynamic section to look them up in.
struct elf_info {
    uint64_t entry;         // AT_ENTRY

    // 1 if the image carried NeoOS's own NOX magic rather than ELF's.
    // Nothing reads it yet: it exists so the first behaviour that should
    // apply only to NeoOS-native binaries has somewhere to hang, without
    // having to re-read the file to find out.
    int is_nox;

    // The program header table AS MAPPED, not as it sits in the file.
    // Computed from the PT_LOAD segment that contains e_phoff, because
    // that is the only thing that relates a file offset to a virtual
    // address. Zero if no loaded segment covers the headers, which is
    // legal ELF and simply means AT_PHDR cannot be supplied.
    uint64_t phdr;          // AT_PHDR
    uint64_t phentsize;     // AT_PHENT
    uint64_t phnum;         // AT_PHNUM

    // The PT_TLS template. `tls_memsz == 0` means the image has no
    // thread-local storage, which is the common case and is not an
    // error. filesz bytes are copied from tls_vaddr and the remaining
    // (memsz - filesz) are zeroed -- .tdata and .tbss respectively.
    uint64_t tls_vaddr;
    uint64_t tls_filesz;
    uint64_t tls_memsz;
    uint64_t tls_align;

    // Every PT_LOAD segment actually mapped, page-aligned exactly as
    // paging_map_into() was called (start/end) with POSIX PROT_*-style
    // bits (kernel/mm/vma.h), not ELF's own p_flags encoding (the two
    // use different bit positions for the same three permissions).
    //
    // Exists so the caller can register a VMA for each one via
    // vma_insert() -- without it, a process's OWN initial image has
    // page-table entries but no vma_find()-visible record at all, so a
    // later mprotect()/munmap() against it silently no-ops instead of
    // taking effect (vma_mprotect_locked only ever sees ranges that
    // came through vma_insert). Real Linux tracks the initial image
    // the same way a later mmap is tracked -- visible in
    // /proc/self/maps. Found via a real crash: a NativeAOT C# binary
    // (the first program in this codebase to mprotect() part of its
    // own loaded image rather than only memory it mmap'd itself) wrote
    // to what it believed was now a writable page and took a write-to-
    // read-only #PF instead, because the mprotect() call that should
    // have unlocked it found no matching VMA and did nothing.
#define ELF_MAX_LOAD_SEGMENTS 16
    int num_segments;
    struct {
        uint64_t start, end;   // page-aligned
        uint32_t prot;         // PROT_READ/WRITE/EXEC (vma.h), always includes PROT_READ
    } segments[ELF_MAX_LOAD_SEGMENTS];
};

// Parses the ELF64 image in `data` (length `size`) and maps its
// PT_LOAD segments into `pml4` (a fresh PML4 from paging_alloc_pml4,
// not yet loaded into CR3), copying each segment's bytes in from
// `data`. Returns 1 on success with *out filled in, 0 on any
// parse/mapping failure (logged to serial).
int elf_load(const uint8_t *data, uint32_t size, uint64_t *pml4,
             struct elf_info *out);

#endif
