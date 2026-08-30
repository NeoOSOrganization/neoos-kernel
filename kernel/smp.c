// kernel/smp.c -- CPU topology, application-processor bringup, IPIs.

#include "smp.h"
#include "serial.h"

// Dense CPU index -> lapic id. cpus[] in cpu_local.h is indexed the same
// way. NEVER index anything by lapic_id directly: APIC ids are sparse
// and can exceed MAX_CPUS on real hardware.
static uint32_t lapic_ids[ACPI_MAX_CPUS];
static int      cpu_count;

void smp_topology_init(const struct acpi_info *info) {
    cpu_count = 0;
    for (uint32_t i = 0; i < info->cpu_count && cpu_count < ACPI_MAX_CPUS; i++) {
        if (info->cpus[i].usable) {
            lapic_ids[cpu_count++] = info->cpus[i].lapic_id;
        }
    }
    serial_write_string("[smp] topology: cpus=");
    serial_write_hex64((uint64_t)cpu_count);
    serial_write_string("\n");
}

int smp_cpu_count(void) { return cpu_count; }

int smp_index_for_lapic(uint32_t lapic_id) {
    for (int i = 0; i < cpu_count; i++) {
        if (lapic_ids[i] == lapic_id) { return i; }
    }
    return -1;
}

uint32_t smp_lapic_for_index(int index) {
    if (index < 0 || index >= cpu_count) { return 0xFFFFFFFFu; }
    return lapic_ids[index];
}

void smp_topology_selftest(void) {
    int n = smp_cpu_count();
    // QEMU is launched with -smp 4 (see the Makefile). Discovering fewer
    // means the MADT walk is dropping Local APIC entries.
    if (n < 2) {
        serial_write_string("[smp] selftest FAILED: fewer than 2 CPUs discovered\n");
        return;
    }
    // Dense indices must round-trip through the lapic_id lookup, since
    // every IPI target is resolved that way.
    for (int i = 0; i < n; i++) {
        uint32_t lapic = smp_lapic_for_index(i);
        if (smp_index_for_lapic(lapic) != i) {
            serial_write_string("[smp] selftest FAILED: lapic_id round-trip\n");
            return;
        }
    }
    if (smp_index_for_lapic(0xDEAD) != -1) {
        serial_write_string("[smp] selftest FAILED: unknown lapic_id not rejected\n");
        return;
    }
    serial_write_string("[smp] topology selftest passed\n");
}
