#include <stdint.h>
#include "arch/idt.h"
#include "arch/gdt.h"

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

extern uint64_t isr_stub_table[256];

static struct idt_entry idt_entries[256];

static void idt_set_gate(int vector, uint64_t handler, uint16_t selector, uint8_t ist, uint8_t type_attr) {
    idt_entries[vector].offset_low = handler & 0xFFFF;
    idt_entries[vector].selector = selector;
    idt_entries[vector].ist = ist & 0x7;
    idt_entries[vector].type_attr = type_attr;
    idt_entries[vector].offset_mid = (handler >> 16) & 0xFFFF;
    idt_entries[vector].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt_entries[vector].zero = 0;
}

void idt_init(void) {
    for (int vector = 0; vector < 256; vector++) {
        uint8_t ist = (vector == 8) ? 1 : 0; // double fault runs on its own IST stack
        idt_set_gate(vector, isr_stub_table[vector], GDT_KERNEL_CODE_SELECTOR, ist, 0x8E);
    }

    idt_load();
}

// Loads the SHARED IDT on the calling CPU. The table itself is built
// once by idt_init; an AP only needs its own lidt.
void idt_load(void) {
    struct idt_ptr idtr = {
        .limit = sizeof(idt_entries) - 1,
        .base = (uint64_t)&idt_entries,
    };

    __asm__ volatile ("lidt %0" :: "m"(idtr));
}
