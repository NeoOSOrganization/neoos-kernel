#include "elf.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "serial.h"

struct elf64_header {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} __attribute__((packed));

struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} __attribute__((packed));

#define ELF_PT_LOAD 1
#define ELF_PT_TLS  7
#define ELF_PF_X    1

static int is_valid_elf64(const struct elf64_header *hdr) {
    return hdr->e_ident[0] == 0x7F && hdr->e_ident[1] == 'E' &&
           hdr->e_ident[2] == 'L' && hdr->e_ident[3] == 'F' &&
           hdr->e_ident[4] == 2; // ELFCLASS64
}

int elf_load(const uint8_t *data, uint32_t size, uint64_t *pml4,
             struct elf_info *out) {
    for (unsigned i = 0; i < sizeof(*out); i++) { ((uint8_t *)out)[i] = 0; }
    if (size < sizeof(struct elf64_header)) {
        serial_write_string("[elf] load FAILED: image too small\n");
        return 0;
    }

    const struct elf64_header *hdr = (const struct elf64_header *)data;
    if (!is_valid_elf64(hdr)) {
        serial_write_string("[elf] load FAILED: not a valid ELF64 image\n");
        return 0;
    }

    for (uint16_t i = 0; i < hdr->e_phnum; i++) {
        const struct elf64_phdr *ph =
            (const struct elf64_phdr *)(data + hdr->e_phoff + (uint64_t)i * hdr->e_phentsize);
        if (ph->p_type == ELF_PT_TLS) {
            // Recorded, never mapped. A PT_TLS segment is a TEMPLATE:
            // its bytes live inside some PT_LOAD segment, and each
            // thread gets its own copy. Mapping it as if it were data
            // would give every thread the same storage, which is the
            // opposite of what it is for.
            out->tls_vaddr  = ph->p_vaddr;
            out->tls_filesz = ph->p_filesz;
            out->tls_memsz  = ph->p_memsz;
            out->tls_align  = ph->p_align ? ph->p_align : 1;
            continue;
        }
        if (ph->p_type != ELF_PT_LOAD) {
            continue;
        }

        // AT_PHDR is a VIRTUAL address, and the only thing that relates
        // a file offset to one is a PT_LOAD segment that covers it.
        // Nothing guarantees the headers are inside a loaded segment --
        // it is a convention every real toolchain follows, not a rule --
        // so this stays zero when they are not, and the auxv simply
        // omits AT_PHDR rather than pointing at nothing.
        if (hdr->e_phoff >= ph->p_offset &&
            hdr->e_phoff <  ph->p_offset + ph->p_filesz) {
            out->phdr = ph->p_vaddr + (hdr->e_phoff - ph->p_offset);
        }

        uint64_t flags = PAGE_WRITABLE | PAGE_USER; // always writable -- no read-only .text/.rodata this milestone
        if (!(ph->p_flags & ELF_PF_X)) {
            flags |= PAGE_NO_EXECUTE;
        }

        uint64_t seg_start = ph->p_vaddr & ~(uint64_t)0xFFF;
        uint64_t seg_end = (ph->p_vaddr + ph->p_memsz + 0xFFF) & ~(uint64_t)0xFFF;

        for (uint64_t page_addr = seg_start; page_addr < seg_end; page_addr += 4096) {
            uint64_t frame_phys = pmm_alloc(0);
            if (!frame_phys) {
                serial_write_string("[elf] load FAILED: out of memory mapping segment\n");
                return 0;
            }

            uint8_t *frame_virt = (uint8_t *)phys_to_virt(frame_phys);
            for (int z = 0; z < 4096; z++) {
                frame_virt[z] = 0;
            }

            // Copy whatever part of this page falls within
            // [p_vaddr, p_vaddr+p_filesz) -- the rest (pure .bss, or
            // the tail of a page beyond filesz) stays zeroed above.
            uint64_t page_end = page_addr + 4096;
            uint64_t copy_start = ph->p_vaddr > page_addr ? ph->p_vaddr : page_addr;
            uint64_t file_end = ph->p_vaddr + ph->p_filesz;
            uint64_t copy_end = file_end < page_end ? file_end : page_end;
            if (copy_end > copy_start) {
                uint64_t src_offset = ph->p_offset + (copy_start - ph->p_vaddr);
                uint64_t dst_offset = copy_start - page_addr;
                for (uint64_t b = 0; b < copy_end - copy_start; b++) {
                    frame_virt[dst_offset + b] = data[src_offset + b];
                }
            }

            paging_map_into(pml4, page_addr, frame_phys, flags);
        }
    }

    out->entry     = hdr->e_entry;
    out->phentsize = hdr->e_phentsize;
    out->phnum     = hdr->e_phnum;
    return 1;
}
