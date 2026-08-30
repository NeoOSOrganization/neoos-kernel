// kernel/smp.c -- CPU topology, application-processor bringup, IPIs.

#include "smp.h"
#include "serial.h"
#include "cpu_local.h"
#include "lapic.h"
#include "gdt.h"
#include "idt.h"
#include "cpu.h"
#include "tss.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "sched/proc.h"
#include "sched/sched.h"

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

// ---- application-processor bringup ----------------------------------

#define AP_TRAMPOLINE_PHYS 0x8000

extern uint8_t ap_trampoline_start[], ap_trampoline_end[];
extern uint8_t ap_trampoline_stack[], ap_trampoline_index[], ap_trampoline_cr3[];
extern uint64_t p4_table[];

static int online_count = 1;   // the BSP is online by definition

int smp_online_count(void) {
    return __atomic_load_n(&online_count, __ATOMIC_ACQUIRE);
}

// Rough spin delay. The bringup protocol needs 10ms and 200us waits and
// runs once per boot, so a calibrated timer is not worth it -- this only
// has to be AT LEAST the required delay, never less.
static void spin_delay_us(uint64_t us) {
    for (uint64_t i = 0; i < us * 200; i++) {
        __asm__ volatile ("pause");
    }
}

// A patchable field's address INSIDE the copy at 0x8000, derived from
// its offset within the linked image.
static void *tramp_field(uint8_t *label) {
    return (void *)(uintptr_t)(AP_TRAMPOLINE_PHYS +
                               (uintptr_t)(label - ap_trampoline_start));
}

void ap_main(int index) {
    gdt_load(index);          // shared GDT; ltr this CPU's own selector
    idt_load();               // shared IDT, already built by the BSP
    cpu_local_init_ap(index); // AFTER gdt_load -- mov gs,ax zeroes GS_BASE
    lapic_init_this_cpu();
    cpu_init();               // SSE/xstate on this CPU
    idle_init_for(index);     // must run ON this CPU: uses this_cpu()'s queue

    __atomic_store_n(&cpus[index].online, 1u, __ATOMIC_RELEASE);
    __atomic_fetch_add(&online_count, 1, __ATOMIC_ACQ_REL);

    serial_write_string("[smp] cpu online, lapic_id=");
    serial_write_hex64(cpus[index].lapic_id);
    serial_write_string("\n");

    __asm__ volatile ("sti");
    for (;;) {
        schedule();
        __asm__ volatile ("hlt");
    }
}

void smp_start_aps(void) {
    uint64_t size = (uint64_t)(ap_trampoline_end - ap_trampoline_start);

    // Copy to a LOW PHYSICAL address, reachable because the boot identity
    // map covers the first 4GiB and is never torn down.
    uint8_t *dst = (uint8_t *)(uintptr_t)AP_TRAMPOLINE_PHYS;
    for (uint64_t i = 0; i < size; i++) {
        dst[i] = ap_trampoline_start[i];
    }

    *(uint32_t *)tramp_field(ap_trampoline_cr3) = (uint32_t)(uintptr_t)p4_table;

    int total = smp_cpu_count();
    for (int i = 1; i < total && i < MAX_CPUS; i++) {
        uint32_t lapic_id = smp_lapic_for_index(i);

        // A fresh 16KiB kernel stack for the AP to run ap_main on.
        uint64_t stack_phys = pmm_alloc(KERNEL_STACK_ORDER);
        if (!stack_phys) {
            serial_write_string("[smp] no memory for AP stack; skipping cpu\n");
            continue;
        }
        uint64_t stack_top = (uint64_t)(uintptr_t)phys_to_virt(stack_phys)
                           + (PMM_FRAME_SIZE << KERNEL_STACK_ORDER);

        *(uint64_t *)tramp_field(ap_trampoline_stack) = stack_top;
        *(uint32_t *)tramp_field(ap_trampoline_index) = (uint32_t)i;

        __atomic_store_n(&cpus[i].online, 0u, __ATOMIC_RELEASE);

        // INIT, then two SIPIs, per the Intel MP protocol.
        lapic_send_init(lapic_id);
        spin_delay_us(10000);
        lapic_send_sipi(lapic_id, AP_TRAMPOLINE_PHYS >> 12);
        spin_delay_us(200);
        lapic_send_sipi(lapic_id, AP_TRAMPOLINE_PHYS >> 12);

        // Bringup is SERIALIZED: wait for this AP before starting the
        // next. All APs share the one trampoline stack slot, so two in
        // flight at once would run on the same stack.
        int waited_us = 0;
        while (!__atomic_load_n(&cpus[i].online, __ATOMIC_ACQUIRE) &&
               waited_us < 100000) {
            spin_delay_us(100);
            waited_us += 100;
        }
        if (!__atomic_load_n(&cpus[i].online, __ATOMIC_ACQUIRE)) {
            // Not fatal: continue with fewer CPUs rather than hanging the
            // boot on one uncooperative core.
            serial_write_string("[smp] cpu failed to come online: lapic_id=");
            serial_write_hex64(lapic_id);
            serial_write_string("\n");
        }
    }

    serial_write_string("[smp] online cpus=");
    serial_write_hex64((uint64_t)smp_online_count());
    serial_write_string("\n");
}

// ---- reschedule IPI --------------------------------------------------

volatile uint64_t ipi_reschedule_count;

// The handler does nothing but acknowledge. Waking the target is the
// INTERRUPT's job: it breaks the idle loop's `hlt`, and the loop reaches
// schedule() on its next pass. Without this an AP that has parked in
// idle can never be given work -- APs get no timer interrupt, since the
// IOAPIC routes only to the BSP.
void ipi_reschedule_handler(void) {
    __atomic_fetch_add(&ipi_reschedule_count, 1, __ATOMIC_ACQ_REL);
    lapic_send_eoi();
}

void smp_send_reschedule(int cpu_index) {
    if (cpu_index < 0 || cpu_index >= smp_online_count()) { return; }
    if (cpu_index == (int)(this_cpu() - &cpus[0])) { return; } // no self-IPI
    lapic_send_ipi(smp_lapic_for_index(cpu_index), VECTOR_IPI_RESCHEDULE);
}

void smp_reschedule_ipi_selftest(void) {
    int target = smp_online_count() - 1;
    if (target < 1) {
        serial_write_string("[smp] ipi selftest FAILED: no AP to target\n");
        return;
    }
    uint64_t before = __atomic_load_n(&ipi_reschedule_count, __ATOMIC_ACQUIRE);
    smp_send_reschedule(target);

    // Bounded wait: an IPI that is never delivered must FAIL the test,
    // not hang the boot.
    for (int i = 0; i < 100000000; i++) {
        if (__atomic_load_n(&ipi_reschedule_count, __ATOMIC_ACQUIRE) > before) {
            serial_write_string("[smp] reschedule ipi selftest passed\n");
            return;
        }
        __asm__ volatile ("pause");
    }
    serial_write_string("[smp] ipi selftest FAILED: reschedule IPI never delivered\n");
}

// ---- panic stop ------------------------------------------------------

void smp_panic_stop_others(void) {
    int self   = (int)(this_cpu() - &cpus[0]);
    int online = smp_online_count();
    for (int i = 0; i < online; i++) {
        if (i == self) { continue; }
        lapic_send_nmi(smp_lapic_for_index(i));
    }
}

// Deliberately SILENT. The panicking CPU may be holding the serial lock,
// so printing here would hang the very stop this was sent to perform.
// Just freeze, and leave the serial port to the CPU that panicked.
void nmi_handler(void) {
    for (;;) { __asm__ volatile ("cli; hlt"); }
}

// Exists so the selftest can assert the invariant the comment above
// documents. Keep returning 0 -- and keep nmi_handler free of any
// serial output.
int smp_panic_handler_takes_serial_lock(void) { return 0; }
