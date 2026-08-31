#include "dev/ioapic.h"
#include "mm/paging.h"

#define IOAPIC_REGSEL 0
#define IOAPIC_REGWIN 4 // 32-bit-word index of the IOWIN register (byte offset 0x10)

static volatile uint32_t *ioapic_base;

static uint32_t ioapic_read(uint8_t reg) {
    ioapic_base[IOAPIC_REGSEL] = reg;
    return ioapic_base[IOAPIC_REGWIN];
}

static void ioapic_write(uint8_t reg, uint32_t value) {
    ioapic_base[IOAPIC_REGSEL] = reg;
    ioapic_base[IOAPIC_REGWIN] = value;
}

void ioapic_init(uint32_t address) {
    ioapic_base = (volatile uint32_t *)phys_to_virt(address);
    (void)ioapic_read(0x00); // touch IOAPICID to confirm the MMIO mapping is live
}

void ioapic_set_redirection(uint8_t pin, uint8_t vector, uint8_t polarity, uint8_t trigger, uint8_t dest_apic_id) {
    uint32_t low = vector;
    if (polarity) {
        low |= (1u << 13); // active-low
    }
    if (trigger) {
        low |= (1u << 15); // level-triggered
    }

    uint8_t reg = 0x10 + pin * 2;
    ioapic_write(reg, low);
    ioapic_write(reg + 1, (uint32_t)dest_apic_id << 24);
}
