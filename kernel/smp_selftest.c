// kernel/smp_selftest.c -- selftests that only mean anything with more
// than one CPU. Kept out of smp.c so the bringup code stays readable.

#include "smp.h"
#include "cpu_local.h"
#include "lock.h"
#include "serial.h"
#include "sched/proc.h"

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
