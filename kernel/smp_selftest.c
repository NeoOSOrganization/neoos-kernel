// kernel/smp_selftest.c -- selftests that only mean anything with more
// than one CPU. Kept out of smp.c so the bringup code stays readable.

#include "smp.h"
#include "cpu_local.h"
#include "lock.h"
#include "serial.h"
#include "sched/proc.h"
#include "tss.h"
#include "gdt.h"

void runqueue_lock_selftest(void) {
    struct cpu *c = this_cpu();
    if (c->ready_lock.rank != LOCK_RANK_RUNQUEUE) {
        serial_write_string("[runq] selftest FAILED: lock rank wrong\n");
        return;
    }
    // ready_count must track the queue, since work stealing picks its
    // victim by comparing counts without walking the lists.
    uint64_t f = spin_lock_irqsave(&c->ready_lock);
    uint32_t counted = 0;
    for (struct thread *t = c->ready_head; t; t = t->next) { counted++; }
    uint32_t claimed = c->ready_count;
    spin_unlock_irqrestore(&c->ready_lock, f);

    if (counted != claimed) {
        serial_write_string("[runq] selftest FAILED: ready_count disagrees with the list\n");
        return;
    }
    serial_write_string("[runq] selftest passed\n");
}

void cpu_local_selftest(void) {
    // Every CPU must get a DISTINCT TSS selector: two CPUs sharing one
    // TSS share rsp0, and the second ring-3 entry lands on the first
    // CPU's kernel stack.
    for (int i = 0; i < MAX_CPUS; i++) {
        for (int j = i + 1; j < MAX_CPUS; j++) {
            if (gdt_tss_selector(i) == gdt_tss_selector(j)) {
                serial_write_string("[cpu] selftest FAILED: duplicate TSS selector\n");
                return;
            }
        }
    }
    // Each CPU also needs its OWN double-fault stack, or two concurrent
    // faults overwrite each other's frame.
    for (int i = 0; i < MAX_TSS; i++) {
        for (int j = i + 1; j < MAX_TSS; j++) {
            if (tss[i].ist1 == tss[j].ist1) {
                serial_write_string("[cpu] selftest FAILED: shared IST1 stack\n");
                return;
            }
        }
    }
    // The BSP's block must be reachable through GS and self-consistent.
    struct cpu *c = this_cpu();
    if (c->self != c) {
        serial_write_string("[cpu] selftest FAILED: gs:0 does not point at itself\n");
        return;
    }
    if (c->tss != &tss[0]) {
        serial_write_string("[cpu] selftest FAILED: BSP not bound to tss[0]\n");
        return;
    }
    serial_write_string("[cpu] local selftest passed\n");
}

void smp_online_selftest(void) {
    int discovered = smp_cpu_count();
    int online     = smp_online_count();
    // The assertion that fails on the pre-SMP kernel: an application
    // processor must actually have come online.
    if (online < 2) {
        serial_write_string("[smp] selftest FAILED: no application processor came online\n");
        return;
    }
    if (online > discovered) {
        serial_write_string("[smp] selftest FAILED: more online than discovered\n");
        return;
    }
    // Each online CPU must have its own idle thread and its own TSS.
    for (int i = 0; i < online; i++) {
        if (!cpus[i].idle) {
            serial_write_string("[smp] selftest FAILED: cpu without an idle thread\n");
            return;
        }
        if (cpus[i].tss != &tss[i]) {
            serial_write_string("[smp] selftest FAILED: cpu not bound to its own tss\n");
            return;
        }
    }
    serial_write_string("[smp] online selftest passed\n");
}
