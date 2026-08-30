#include "acpi.h"
#include "serial.h"
#include "mm/paging.h"

struct acpi_rsdp {
    char signature[8];
    uint8_t checksum;
    char oem_id[6];
    uint8_t revision;
    uint32_t rsdt_address;
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t extended_checksum;
    uint8_t reserved[3];
} __attribute__((packed));

struct acpi_sdt_header {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

struct acpi_madt {
    struct acpi_sdt_header header;
    uint32_t local_apic_address;
    uint32_t flags;
} __attribute__((packed));

struct madt_entry_header {
    uint8_t type;
    uint8_t length;
} __attribute__((packed));

// MADT type 0. `flags` bit 0 = Enabled, bit 1 = Online Capable; either
// one means the OS may start this CPU.
struct madt_lapic {
    struct madt_entry_header header;
    uint8_t  acpi_processor_id;
    uint8_t  apic_id;
    uint32_t flags;
} __attribute__((packed));

struct madt_ioapic {
    struct madt_entry_header header;
    uint8_t ioapic_id;
    uint8_t reserved;
    uint32_t ioapic_address;
    uint32_t gsi_base;
} __attribute__((packed));

struct madt_iso {
    struct madt_entry_header header;
    uint8_t bus_source;
    uint8_t irq_source;
    uint32_t gsi;
    uint16_t flags;
} __attribute__((packed));

static uint8_t sum_bytes(const void *ptr, uint32_t length) {
    uint8_t sum = 0;
    const uint8_t *bytes = (const uint8_t *)ptr;
    for (uint32_t i = 0; i < length; i++) {
        sum = (uint8_t)(sum + bytes[i]);
    }
    return sum;
}

static int is_rsdp_signature(const struct acpi_rsdp *candidate) {
    static const char signature[8] = { 'R', 'S', 'D', ' ', 'P', 'T', 'R', ' ' };
    for (int i = 0; i < 8; i++) {
        if (candidate->signature[i] != signature[i]) {
            return 0;
        }
    }
    return 1;
}

static struct acpi_rsdp *find_rsdp_in_range(uint32_t start, uint32_t end) {
    for (uint32_t addr = start; addr < end; addr += 16) {
        struct acpi_rsdp *candidate = (struct acpi_rsdp *)phys_to_virt(addr);
        if (is_rsdp_signature(candidate) && sum_bytes(candidate, 20) == 0) {
            return candidate;
        }
    }
    return 0;
}

static struct acpi_rsdp *find_rsdp(void) {
    // GCC statically flags this fixed low-physical-memory read as
    // "near null" and can't know it's a valid BIOS Data Area access.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
    uint16_t ebda_segment = *(volatile uint16_t *)phys_to_virt(0x40E);
#pragma GCC diagnostic pop
    uint32_t ebda_addr = (uint32_t)ebda_segment << 4;

    struct acpi_rsdp *rsdp = find_rsdp_in_range(ebda_addr, ebda_addr + 1024);
    if (rsdp) {
        return rsdp;
    }
    return find_rsdp_in_range(0xE0000, 0x100000);
}

static int is_apic_signature(const struct acpi_sdt_header *header) {
    return header->signature[0] == 'A' && header->signature[1] == 'P' &&
           header->signature[2] == 'I' && header->signature[3] == 'C';
}

static struct acpi_madt *find_madt_via_xsdt(uint64_t xsdt_address) {
    struct acpi_sdt_header *xsdt = (struct acpi_sdt_header *)phys_to_virt(xsdt_address);
    uint64_t *entries = (uint64_t *)((uint8_t *)xsdt + sizeof(struct acpi_sdt_header));
    uint32_t entry_count = (xsdt->length - sizeof(struct acpi_sdt_header)) / sizeof(uint64_t);

    for (uint32_t i = 0; i < entry_count; i++) {
        struct acpi_sdt_header *table = (struct acpi_sdt_header *)phys_to_virt(entries[i]);
        if (is_apic_signature(table)) {
            return (struct acpi_madt *)table;
        }
    }
    return 0;
}

static struct acpi_madt *find_madt_via_rsdt(uint32_t rsdt_address) {
    struct acpi_sdt_header *rsdt = (struct acpi_sdt_header *)phys_to_virt(rsdt_address);
    uint32_t *entries = (uint32_t *)((uint8_t *)rsdt + sizeof(struct acpi_sdt_header));
    uint32_t entry_count = (rsdt->length - sizeof(struct acpi_sdt_header)) / sizeof(uint32_t);

    for (uint32_t i = 0; i < entry_count; i++) {
        struct acpi_sdt_header *table = (struct acpi_sdt_header *)phys_to_virt(entries[i]);
        if (is_apic_signature(table)) {
            return (struct acpi_madt *)table;
        }
    }
    return 0;
}

static void parse_madt(struct acpi_madt *madt, struct acpi_info *info) {
    info->lapic_address = madt->local_apic_address;
    info->irq0_gsi = 0;
    info->irq0_polarity = 0;
    info->irq0_trigger = 0;
    info->irq1_gsi = 1;
    info->irq1_polarity = 0;
    info->irq1_trigger = 0;
    info->cpu_count = 0;

    uint8_t *ptr = (uint8_t *)madt + sizeof(struct acpi_madt);
    uint8_t *end = (uint8_t *)madt + madt->header.length;

    while (ptr < end) {
        struct madt_entry_header *entry = (struct madt_entry_header *)ptr;

        if (entry->type == 0) {
            struct madt_lapic *lapic = (struct madt_lapic *)ptr;
            if ((lapic->flags & 0x3) && info->cpu_count < ACPI_MAX_CPUS) {
                struct acpi_cpu *c = &info->cpus[info->cpu_count++];
                c->acpi_id  = lapic->acpi_processor_id;
                c->lapic_id = lapic->apic_id;
                c->usable   = 1;
            }
        } else if (entry->type == 1) {
            struct madt_ioapic *ioapic = (struct madt_ioapic *)ptr;
            info->ioapic_address = ioapic->ioapic_address;
            info->ioapic_gsi_base = ioapic->gsi_base;
        } else if (entry->type == 2) {
            struct madt_iso *iso = (struct madt_iso *)ptr;
            uint8_t polarity_bits = iso->flags & 0x3;
            uint8_t trigger_bits = (iso->flags >> 2) & 0x3;
            uint8_t polarity = (polarity_bits == 3) ? 1 : 0;
            uint8_t trigger = (trigger_bits == 3) ? 1 : 0;

            if (iso->irq_source == 0) {
                info->irq0_gsi = (uint8_t)iso->gsi;
                info->irq0_polarity = polarity;
                info->irq0_trigger = trigger;
            } else if (iso->irq_source == 1) {
                info->irq1_gsi = (uint8_t)iso->gsi;
                info->irq1_polarity = polarity;
                info->irq1_trigger = trigger;
            }
        }

        ptr += entry->length;
    }
}

void acpi_find_madt(struct acpi_info *info) {
    struct acpi_rsdp *rsdp = find_rsdp();

    struct acpi_madt *madt;
    if (rsdp->revision >= 2) {
        madt = find_madt_via_xsdt(rsdp->xsdt_address);
    } else {
        madt = find_madt_via_rsdt(rsdp->rsdt_address);
    }

    parse_madt(madt, info);

    serial_write_string("[acpi] lapic="); serial_write_hex64(info->lapic_address);
    serial_write_string(" ioapic="); serial_write_hex64(info->ioapic_address);
    serial_write_string(" ioapic_gsi_base="); serial_write_hex64(info->ioapic_gsi_base);
    serial_write_string("\n[acpi] irq0_gsi="); serial_write_hex64(info->irq0_gsi);
    serial_write_string(" irq1_gsi="); serial_write_hex64(info->irq1_gsi);
    serial_write_string("\n[acpi] cpus="); serial_write_hex64(info->cpu_count);
    serial_write_string("\n");
}
