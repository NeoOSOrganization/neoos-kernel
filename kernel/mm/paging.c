#include "mm/paging.h"
#include "mm/pmm.h"
#include "smp/tlb.h"
#include "dev/serial.h"

#define PAGE_HUGE (1ULL << 7) // 2MiB page at the PD level

#define PHYSMAP_SIZE_BYTES (4ULL * 1024 * 1024 * 1024) // first 4GiB: all supported RAM plus the sub-4GiB MMIO hole (LAPIC/IOAPIC)
#define PHYSMAP_PML4_INDEX 256

#define PT_INDEX(va)   (((va) >> 12) & 0x1FF)
#define PD_INDEX(va)   (((va) >> 21) & 0x1FF)
#define PDPT_INDEX(va) (((va) >> 30) & 0x1FF)
#define PML4_INDEX(va) (((va) >> 39) & 0x1FF)

extern uint64_t p4_table[512]; // boot.asm's live PML4 -- see boot/boot.asm

// paging_init() calls alloc_table_frame() to build the physmap itself,
// before that physmap exists -- phys_to_virt() can't be used yet at
// that point, only the identity map boot.asm already set up (valid
// for whatever low physical range pmm hands out this early). Once the
// physmap is installed, every later caller (paging_alloc_pml4(),
// table_entry()) must use it instead: the identity map's own coverage
// isn't guaranteed to extend to frames pmm hands out much later, once
// process page tables are being built.
static int physmap_installed = 0;

static uint64_t alloc_table_frame(void) {
    uint64_t phys = pmm_alloc(0);
    uint64_t *table = physmap_installed ? (uint64_t *)phys_to_virt(phys) : (uint64_t *)(uintptr_t)phys;
    for (int i = 0; i < 512; i++) {
        table[i] = 0;
    }
    return phys;
}

// Walks one level, allocating a fresh table if `create` is set and the
// entry isn't present yet. Assumes 4KiB-page-tree structure throughout
// (not valid on huge-page-mapped regions -- see paging.h).
static uint64_t *table_entry(uint64_t *table, unsigned index, int create, uint64_t create_flags) {
    if (!(table[index] & PAGE_PRESENT)) {
        if (!create) {
            return 0;
        }
        uint64_t new_table_phys = alloc_table_frame();
        table[index] = new_table_phys | create_flags;
    }
    uint64_t next_phys = table[index] & PAGE_ADDR_MASK;
    return (uint64_t *)phys_to_virt(next_phys);
}

uint64_t paging_alloc_pml4(void) {
    return alloc_table_frame();
}

int paging_map_into(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t default_flags = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    uint64_t *pdpt = table_entry(pml4, PML4_INDEX(virt), 1, default_flags);
    uint64_t *pd   = table_entry(pdpt, PDPT_INDEX(virt), 1, default_flags);
    uint64_t *pt   = table_entry(pd, PD_INDEX(virt), 1, default_flags);

    pt[PT_INDEX(virt)] = (phys & PAGE_ADDR_MASK) | flags | PAGE_PRESENT;
    __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
    return 0;
}

int paging_map(uint64_t virt, uint64_t phys, uint64_t flags) {
    return paging_map_into(p4_table, virt, phys, flags);
}

void paging_unmap(uint64_t virt) {
    uint64_t *pdpt = table_entry(p4_table, PML4_INDEX(virt), 0, 0);
    uint64_t *pd   = pdpt ? table_entry(pdpt, PDPT_INDEX(virt), 0, 0) : 0;
    uint64_t *pt   = pd ? table_entry(pd, PD_INDEX(virt), 0, 0) : 0;
    if (pt) {
        pt[PT_INDEX(virt)] = 0;
        __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
    }
}

// Clears one PTE in an arbitrary address space, optionally freeing the
// frame it pointed at. Returns 1 if a mapping was actually removed, 0
// if the address was not mapped. Used to tear down a thread's user
// stack when the thread exits (see thread_stack_free).
int paging_unmap_from(uint64_t *pml4, uint64_t virt, int free_frame) {
    uint64_t *pdpt = table_entry(pml4, PML4_INDEX(virt), 0, 0);
    uint64_t *pd   = pdpt ? table_entry(pdpt, PDPT_INDEX(virt), 0, 0) : 0;
    uint64_t *pt   = pd ? table_entry(pd, PD_INDEX(virt), 0, 0) : 0;
    if (!pt || !(pt[PT_INDEX(virt)] & PAGE_PRESENT)) {
        return 0;
    }
    uint64_t phys = pt[PT_INDEX(virt)] & PAGE_ADDR_MASK;
    pt[PT_INDEX(virt)] = 0;
    __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
    if (free_frame) {
        // NOT pmm_free: the frame must not be reusable until every CPU
        // that might hold a stale TLB entry for it has acknowledged a
        // shootdown. The local invlpg above only covers THIS CPU.
        tlb_defer_free(phys, 0);
    }
    return 1;
}

int user_range_writable(uint64_t addr, uint64_t len) {
    if (len == 0) { return 1; }
    // Reject anything that is not a canonical low-half (user) address,
    // including wraparound.
    if (addr + len < addr) { return 0; }
    if (addr + len > USER_ADDR_LIMIT) { return 0; }

    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    uint64_t pml4_phys = cr3 & PAGE_ADDR_MASK;
    uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);

    for (uint64_t v = addr & ~0xFFFULL; v < addr + len; v += PMM_FRAME_SIZE) {
        uint64_t *pdpt = table_entry(pml4, PML4_INDEX(v), 0, 0);
        uint64_t *pd   = pdpt ? table_entry(pdpt, PDPT_INDEX(v), 0, 0) : 0;
        uint64_t *pt   = pd   ? table_entry(pd,   PD_INDEX(v),   0, 0) : 0;
        if (!pt) { return 0; }

        uint64_t e = pt[PT_INDEX(v)];
        if ((e & (PAGE_PRESENT | PAGE_USER)) != (PAGE_PRESENT | PAGE_USER)) {
            return 0;
        }
        if (!(e & PAGE_WRITABLE)) {
            // A present, user, read-only page marked PAGE_COW is a
            // fork() copy-on-write page: writing it IS legal, but the
            // kernel cannot just do it -- a CPL0 write faults with
            // user=0, which paging_handle_cow_fault's present+write+user
            // test rejects, so it would reach exception_dump_and_halt
            // and take the machine down. Break the sharing here instead.
            // A read-only page WITHOUT PAGE_COW is a real W^X segment and
            // genuinely not writable: report that, like Linux's -EFAULT.
            if (!(e & PAGE_COW)) { return 0; }
            if (!paging_handle_cow_fault(pml4_phys, v)) { return 0; }
        }
    }
    return 1;
}

// Like user_range_writable, but only checks that the range is mapped
// and user-accessible -- a read-only page (a string literal in .rodata,
// a PROT_READ mmap) passes. Use this to validate a buffer the kernel
// will only READ from (send(), a pathname, an iovec's source).
int user_range_readable(uint64_t addr, uint64_t len) {
    if (len == 0) { return 1; }
    if (addr + len < addr) { return 0; }
    if (addr + len > USER_ADDR_LIMIT) { return 0; }

    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    uint64_t *pml4 = (uint64_t *)phys_to_virt(cr3 & PAGE_ADDR_MASK);

    for (uint64_t v = addr & ~0xFFFULL; v < addr + len; v += PMM_FRAME_SIZE) {
        uint64_t *pdpt = table_entry(pml4, PML4_INDEX(v), 0, 0);
        uint64_t *pd   = pdpt ? table_entry(pdpt, PDPT_INDEX(v), 0, 0) : 0;
        uint64_t *pt   = pd   ? table_entry(pd,   PD_INDEX(v),   0, 0) : 0;
        if (!pt) { return 0; }
        uint64_t e = pt[PT_INDEX(v)];
        if ((e & (PAGE_PRESENT | PAGE_USER)) != (PAGE_PRESENT | PAGE_USER)) {
            return 0;
        }
    }
    return 1;
}

// Same walk, but through the address space CURRENTLY IN CR3 rather than
// the kernel's own p4_table. paging_translate below is no use for a
// user pointer: user mappings do not exist in p4_table at all.
// Unlike table_entry, this handles huge pages. It has to: the kernel's
// own higher-half alias and the physmap are both 2MiB-mapped, so a walk
// that assumed 4KiB entries throughout would read a 2MiB PD entry as if
// it pointed at a page table and return a physical address derived from
// whatever data happened to be in that page. PAGE_HUGE is the PS bit,
// meaningful in a PDPT (1GiB) or PD (2MiB) entry.
uint64_t paging_translate_in(uint64_t pml4_phys, uint64_t virt) {
    uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys & PAGE_ADDR_MASK);

    uint64_t e = pml4[PML4_INDEX(virt)];
    if (!(e & PAGE_PRESENT)) { return 0; }

    uint64_t *pdpt = (uint64_t *)phys_to_virt(e & PAGE_ADDR_MASK);
    e = pdpt[PDPT_INDEX(virt)];
    if (!(e & PAGE_PRESENT)) { return 0; }
    if (e & PAGE_HUGE) {   // 1GiB page
        return (e & PAGE_ADDR_MASK) | (virt & 0x3FFFFFFFULL);
    }

    uint64_t *pd = (uint64_t *)phys_to_virt(e & PAGE_ADDR_MASK);
    e = pd[PD_INDEX(virt)];
    if (!(e & PAGE_PRESENT)) { return 0; }
    if (e & PAGE_HUGE) {   // 2MiB page
        return (e & PAGE_ADDR_MASK) | (virt & 0x1FFFFFULL);
    }

    uint64_t *pt = (uint64_t *)phys_to_virt(e & PAGE_ADDR_MASK);
    e = pt[PT_INDEX(virt)];
    if (!(e & PAGE_PRESENT)) { return 0; }
    return (e & PAGE_ADDR_MASK) | (virt & 0xFFFULL);
}

uint64_t paging_translate_current(uint64_t virt) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return paging_translate_in(cr3, virt);
}

uint64_t paging_translate(uint64_t virt) {
    uint64_t *pdpt = table_entry(p4_table, PML4_INDEX(virt), 0, 0);
    uint64_t *pd   = pdpt ? table_entry(pdpt, PDPT_INDEX(virt), 0, 0) : 0;
    uint64_t *pt   = pd ? table_entry(pd, PD_INDEX(virt), 0, 0) : 0;
    if (!pt || !(pt[PT_INDEX(virt)] & PAGE_PRESENT)) {
        return 0;
    }
    return (pt[PT_INDEX(virt)] & PAGE_ADDR_MASK) | (virt & 0xFFF);
}

void paging_init(void) {
    uint64_t pdpt_phys = alloc_table_frame();
    uint64_t *pdpt = (uint64_t *)(uintptr_t)pdpt_phys;

    uint64_t total_pages = PHYSMAP_SIZE_BYTES / (2 * 1024 * 1024);
    uint64_t pages_mapped = 0;
    for (uint64_t pdpt_index = 0; pages_mapped < total_pages; pdpt_index++) {
        uint64_t pd_phys = alloc_table_frame();
        uint64_t *pd = (uint64_t *)(uintptr_t)pd_phys;

        for (unsigned pd_index = 0; pd_index < 512 && pages_mapped < total_pages; pd_index++, pages_mapped++) {
            uint64_t page_phys = pages_mapped * (2ULL * 1024 * 1024);
            pd[pd_index] = page_phys | PAGE_PRESENT | PAGE_WRITABLE | PAGE_HUGE;
        }

        pdpt[pdpt_index] = pd_phys | PAGE_PRESENT | PAGE_WRITABLE;
    }

    p4_table[PHYSMAP_PML4_INDEX] = pdpt_phys | PAGE_PRESENT | PAGE_WRITABLE;
    physmap_installed = 1;

    serial_write_string("[paging] physmap installed: base=");
    serial_write_hex64(PHYSMAP_BASE);
    serial_write_string(" size=");
    serial_write_hex64(PHYSMAP_SIZE_BYTES);
    serial_write_string("\n");
}

// Frees every user-mapped frame and page-table frame reachable from
// pml4_phys, then the PML4 frame itself. The three shared kernel
// entries (identity map, physmap, kernel higher-half alias -- see
// spawn()'s pml4[0]/[256]/[511] setup) are never walked into or
// freed: they point at kernel-owned tables no process owns.
// pmm_free() is refcount-aware (see pmm.c) -- a COW-shared frame only
// actually returns to the allocator once every sharer has released
// it, so calling this on a fork()'d child's or parent's address space
// is always safe regardless of sharing.
//
// The caller MUST NOT still have pml4_phys loaded in CR3: pmm's buddy
// allocator keeps each free block's next/prev links in the block's own
// first 16 bytes, so freeing the PML4 frame overwrites pml4[0] and
// pml4[1] -- pml4[0] being the low identity map that pmm dereferences
// free blocks through. See task_exit(), which switches to p4_table
// first.
void free_address_space(uint64_t pml4_phys) {
    uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);

    for (unsigned i4 = 0; i4 < 512; i4++) {
        if (i4 == 0 || i4 == PHYSMAP_PML4_INDEX || i4 == 511) {
            continue; // shared kernel entries -- not owned by this address space
        }
        if (!(pml4[i4] & PAGE_PRESENT)) {
            continue;
        }
        uint64_t pdpt_phys = pml4[i4] & PAGE_ADDR_MASK;
        uint64_t *pdpt = (uint64_t *)phys_to_virt(pdpt_phys);

        for (unsigned i3 = 0; i3 < 512; i3++) {
            if (!(pdpt[i3] & PAGE_PRESENT)) {
                continue;
            }
            uint64_t pd_phys = pdpt[i3] & PAGE_ADDR_MASK;
            uint64_t *pd = (uint64_t *)phys_to_virt(pd_phys);

            for (unsigned i2 = 0; i2 < 512; i2++) {
                if (!(pd[i2] & PAGE_PRESENT)) {
                    continue;
                }
                uint64_t pt_phys = pd[i2] & PAGE_ADDR_MASK;
                uint64_t *pt = (uint64_t *)phys_to_virt(pt_phys);

                for (unsigned i1 = 0; i1 < 512; i1++) {
                    if (pt[i1] & PAGE_PRESENT) {
                        pmm_free(pt[i1] & PAGE_ADDR_MASK, 0);
                    }
                }
                pmm_free(pt_phys, 0);
            }
            pmm_free(pd_phys, 0);
        }
        pmm_free(pdpt_phys, 0);
    }

    pmm_free(pml4_phys, 0);
}

// Called from isr.c on a #PF with error_code bits present=1, write=1,
// user=1. Since fork() is the only thing in this kernel that ever marks
// a user PTE read-only, any such fault is assumed to be copy-on-write,
// never a genuine permission violation -- there is no separate "real
// read-only segment" concept yet (elf_load() always maps
// PAGE_WRITABLE). Returns 1 if handled (safe to return to the faulting
// instruction, which will now succeed), 0 if this wasn't actually a
// recognized COW fault (caller should fall through to the existing
// fatal exception path -- covers genuine bugs, wild pointers, and
// non-present accesses unchanged).
int paging_handle_cow_fault(uint64_t pml4_phys, uint64_t fault_addr) {
    uint64_t *pml4 = (uint64_t *)phys_to_virt(pml4_phys);
    uint64_t *pdpt = table_entry(pml4, PML4_INDEX(fault_addr), 0, 0);
    uint64_t *pd   = pdpt ? table_entry(pdpt, PDPT_INDEX(fault_addr), 0, 0) : 0;
    uint64_t *pt   = pd ? table_entry(pd, PD_INDEX(fault_addr), 0, 0) : 0;
    if (!pt) {
        return 0;
    }

    unsigned pt_index = PT_INDEX(fault_addr);
    uint64_t entry = pt[pt_index];
    if (!(entry & PAGE_PRESENT) || (entry & PAGE_WRITABLE)) {
        return 0; // not present, or already writable -- not a COW fault
    }
    if (!(entry & PAGE_COW)) {
        // Present, read-only, and NOT marked copy-on-write: a genuine
        // W^X page (.text/.rodata). Writing it is a real access
        // violation -- let the caller fall through to SIGSEGV.
        return 0;
    }

    uint64_t old_phys = entry & PAGE_ADDR_MASK;
    // Whatever happens below, the page stops being copy-on-write for
    // this address space, so drop the marker along with re-granting write.
    uint64_t flags_no_addr = entry & ~PAGE_ADDR_MASK & ~PAGE_PRESENT & ~PAGE_COW;

    if (pmm_frame_refcount(old_phys) == 1) {
        // Sole remaining owner -- no copy needed, just re-enable write.
        pt[pt_index] = (entry & ~PAGE_COW) | PAGE_WRITABLE;
    } else {
        uint64_t new_phys = pmm_alloc(0);
        if (!new_phys) {
            return 0; // out of memory -- caller's fatal path will report this fault
        }
        uint8_t *src = (uint8_t *)phys_to_virt(old_phys);
        uint8_t *dst = (uint8_t *)phys_to_virt(new_phys);
        for (int i = 0; i < 4096; i++) {
            dst[i] = src[i];
        }
        pt[pt_index] = (new_phys & PAGE_ADDR_MASK) | flags_no_addr | PAGE_PRESENT | PAGE_WRITABLE;
        pmm_free(old_phys, 0); // drops this task's share
    }

    __asm__ volatile ("invlpg (%0)" :: "r"(fault_addr) : "memory");
    return 1;
}

#define PAGING_SELFTEST_VA 0xFFFF900000000000ULL

void paging_selftest(void) {
    uint64_t scratch_phys = pmm_alloc(0);
    if (!scratch_phys) {
        serial_write_string("[paging] selftest FAILED: pmm_alloc returned 0\n");
        return;
    }

    if (paging_map(PAGING_SELFTEST_VA, scratch_phys, PAGE_WRITABLE) != 0) {
        serial_write_string("[paging] selftest FAILED: paging_map error\n");
        return;
    }

    volatile uint8_t *scratch = (volatile uint8_t *)(uintptr_t)PAGING_SELFTEST_VA;
    *scratch = 0x42;
    if (*scratch != 0x42) {
        serial_write_string("[paging] selftest FAILED: pattern mismatch through new mapping\n");
        return;
    }

    if (paging_translate(PAGING_SELFTEST_VA) != scratch_phys) {
        serial_write_string("[paging] selftest FAILED: translate did not round-trip\n");
        return;
    }

    paging_unmap(PAGING_SELFTEST_VA);
    if (paging_translate(PAGING_SELFTEST_VA) != 0) {
        serial_write_string("[paging] selftest FAILED: translate still resolves after unmap\n");
        return;
    }

    pmm_free(scratch_phys, 0);
    serial_write_string("[paging] selftest passed\n");
}
