#ifndef NEOOS_ACPI_H
#define NEOOS_ACPI_H

#include <stdint.h>

// 128 fits comfortably inside xAPIC's 8-bit APIC ID field, so no x2APIC
// support is needed. Going beyond 255 would require it.
#define ACPI_MAX_CPUS 128

struct acpi_cpu {
    uint8_t acpi_id;
    uint8_t lapic_id;
    uint8_t usable;   // Enabled, or Online Capable
};

struct acpi_info {
    struct acpi_cpu cpus[ACPI_MAX_CPUS];
    uint32_t        cpu_count;
    uint32_t lapic_address;
    uint32_t ioapic_address;
    uint32_t ioapic_gsi_base;
    uint8_t  irq0_gsi;
    uint8_t  irq0_polarity; // 0 = active-high, 1 = active-low
    uint8_t  irq0_trigger;  // 0 = edge, 1 = level
    uint8_t  irq1_gsi;
    uint8_t  irq1_polarity;
    uint8_t  irq1_trigger;
};

void acpi_find_madt(struct acpi_info *info);

#endif
