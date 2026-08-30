#include <stdint.h>
#include "gdt.h"
#include "tss.h"
#include "cpu_local.h"

struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

// Slots 0-7 keep their original meanings and, crucially, their original
// SELECTOR VALUES. The STAR MSR pins kernel CS/SS adjacency and the
// user code32 -> data -> code64 ordering, and context_switch.asm
// hard-codes the user data selector, so relocating any of them would
// triple-fault on the first ring-3 entry rather than fail the build.
//
// Per-CPU TSS descriptors are therefore APPENDED at slot 8, two 8-byte
// slots each. Slots 3-4 (the old single-CPU TSS at selector 0x18) are
// left zeroed: wasting one descriptor pair costs 16 bytes and buys a
// uniform gdt_tss_selector() formula with no selector churn.
//
// At MAX_CPUS 128 the table is 8 + 2*128 = 264 entries (2112 bytes),
// comfortably inside the GDT limit field's 16 bits.
#define GDT_TSS_FIRST_SLOT 8
static uint64_t gdt_entries[GDT_TSS_FIRST_SLOT + 2 * MAX_CPUS];

uint16_t gdt_tss_selector(int cpu_index) {
    return (uint16_t)((GDT_TSS_FIRST_SLOT + 2 * cpu_index) * 8);
}

extern void gdt_flush(uint64_t gdtr_ptr, uint16_t data_selector,
                       uint16_t code_selector, uint16_t tss_selector);

static void set_tss_descriptor(int slot, uint64_t base, uint32_t limit) {
    uint64_t low = limit & 0xFFFF;
    low |= (base & 0xFFFFFF) << 16;
    low |= (uint64_t)0x89 << 40;              // present, DPL0, type=0x9 (64-bit TSS, available)
    low |= (uint64_t)((limit >> 16) & 0xF) << 48;
    low |= ((base >> 24) & 0xFF) << 56;

    uint64_t high = (base >> 32) & 0xFFFFFFFF;

    gdt_entries[slot]     = low;
    gdt_entries[slot + 1] = high;
}

void gdt_init(void) {
    gdt_entries[0] = 0;                                                           // null
    gdt_entries[1] = (1ULL << 43) | (1ULL << 44) | (1ULL << 47) | (1ULL << 53);    // kernel code (0x08)
    gdt_entries[2] = (1ULL << 41) | (1ULL << 44) | (1ULL << 47);                   // kernel data (0x10)
    gdt_entries[3] = 0;   // legacy TSS slot, deliberately unused
    gdt_entries[4] = 0;
    gdt_entries[5] = (1ULL << 43) | (1ULL << 44) | (1ULL << 47) | (3ULL << 45);    // user code32 placeholder (0x28)
    gdt_entries[6] = (1ULL << 41) | (1ULL << 44) | (1ULL << 47) | (3ULL << 45);    // user data (0x30)
    gdt_entries[7] = (1ULL << 43) | (1ULL << 44) | (1ULL << 47) | (1ULL << 53) | (3ULL << 45); // user code64 (0x38)

    // One descriptor per CPU, all populated up front: an AP loads the
    // same table and only needs its own selector.
    for (int i = 0; i < MAX_CPUS; i++) {
        set_tss_descriptor(GDT_TSS_FIRST_SLOT + 2 * i, (uint64_t)&tss[i],
                           sizeof(struct tss_entry) - 1);
    }

    gdt_load(0);
}

void gdt_load(int cpu_index) {
    struct gdtr gdtr = {
        .limit = sizeof(gdt_entries) - 1,
        .base = (uint64_t)&gdt_entries,
    };

    gdt_flush((uint64_t)&gdtr, GDT_KERNEL_DATA_SELECTOR,
              GDT_KERNEL_CODE_SELECTOR, gdt_tss_selector(cpu_index));
}
