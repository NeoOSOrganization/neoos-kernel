#ifndef NEOOS_SMP_H
#define NEOOS_SMP_H

#include <stdint.h>
#include "drivers/acpi/acpi.h"

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
void     smp_online_selftest(void);
void     smp_parallel_selftest_start(void);
void     smp_parallel_selftest_check(void);
void     smp_timer_selftest_check(void);
void     smp_steal_selftest_start(void);
void     smp_steal_selftest_check(void);

// Application-processor bringup. Call after every subsystem an AP can
// touch is initialised and locked.
void     smp_start_aps(void);
int      smp_online_count(void);
void     ap_main(int index);

// Expose the user-thread migration counter for test hooks.
uint64_t smp_user_migration_count(void);

// ---- IPIs ------------------------------------------------------------
#define VECTOR_IPI_RESCHEDULE 0xF0

extern volatile uint64_t ipi_reschedule_count;

// Wakes `cpu_index` if it is halted in the idle loop. A no-op when the
// target is this CPU.
void     smp_send_reschedule(int cpu_index);
void     ipi_reschedule_handler(void);
void     smp_reschedule_ipi_selftest(void);

// ---- panic stop ------------------------------------------------------
// There is deliberately no vector constant here: the stop is an NMI
// (vector 2), not a maskable IPI. A VECTOR_IPI_PANIC of 0xF2 used to be
// defined and was never sent by anything -- its only reader was the
// selftest, which therefore passed by checking a constant against
// itself while the real path was broken.

// Freezes every OTHER CPU. Delivered as an NMI, not a maskable vector:
// the whole point is stopping CPUs spinning with interrupts disabled,
// which a normal IPI cannot reach.
void     smp_panic_stop_others(void);
void     nmi_handler(void);
int      smp_panic_handler_takes_serial_lock(void);
int      smp_nmi_selftest(void);
void     panic_stop_selftest(void);

#endif
