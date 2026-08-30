#ifndef NEOOS_SMP_H
#define NEOOS_SMP_H

#include <stdint.h>
#include "acpi.h"

// CPU topology. Everything in the kernel indexes CPUs by a DENSE index
// (0..smp_cpu_count()-1), never by lapic_id: APIC ids are sparse and
// can exceed MAX_CPUS on real hardware. These two functions are the
// only sanctioned way to cross between the two numbering schemes.
void     smp_topology_init(const struct acpi_info *info);
int      smp_cpu_count(void);
int      smp_index_for_lapic(uint32_t lapic_id);   // -1 if unknown
uint32_t smp_lapic_for_index(int index);           // 0xFFFFFFFF if out of range

void     smp_topology_selftest(void);
void     runqueue_lock_selftest(void);
void     cpu_local_selftest(void);

#endif
