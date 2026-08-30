// kernel/smp_selftest.c -- selftests that only mean anything with more
// than one CPU. Kept out of smp.c so the bringup code stays readable.

#include "smp.h"
#include "cpu_local.h"
#include "lock.h"
#include "serial.h"
#include "sched/proc.h"
#include "sched/sched.h"
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

// ---- parallelism -----------------------------------------------------
//
// The test that proves CPUs actually execute work concurrently, rather
// than merely reporting themselves online.

#define PAR_THREADS    8
#define PAR_ITERATIONS 10000

static struct spinlock  par_lock;
static uint64_t         par_counter;
static volatile int     par_done;
static volatile uint8_t par_cpu_seen[MAX_CPUS];

static void par_worker(void) {
    for (int i = 0; i < PAR_ITERATIONS; i++) {
        uint64_t f = spin_lock_irqsave(&par_lock);
        par_counter++;
        spin_unlock_irqrestore(&par_lock, f);
        // Recorded OUTSIDE the lock on purpose: this is about where the
        // work ran, and taking the lock to record it would serialise the
        // very parallelism being measured.
        par_cpu_seen[this_cpu() - &cpus[0]] = 1;
    }
    __atomic_fetch_add((int *)&par_done, 1, __ATOMIC_ACQ_REL);
    thread_exit_self(0);
}

void smp_parallel_selftest_start(void) {
    spin_init(&par_lock, LOCK_RANK_PROCESS, "smp-parallel");
    par_counter = 0;
    par_done    = 0;
    for (int i = 0; i < MAX_CPUS; i++) { par_cpu_seen[i] = 0; }

    int online = smp_online_count();
    for (int i = 0; i < PAR_THREADS; i++) {
        struct thread *t = thread_alloc_kernel(par_worker);
        if (!t) { return; }
        // thread_alloc_kernel already queued it on THIS cpu; move it so
        // the work cannot all land on the BSP by default.
        dequeue_specific(t);
        enqueue_ready_on(i % online, t);
    }
}

// Called from the idle loop; reports once every worker has finished.
void smp_parallel_selftest_check(void) {
    if (__atomic_load_n((int *)&par_done, __ATOMIC_ACQUIRE) != PAR_THREADS) {
        return;   // not finished yet
    }
    static int reported;
    if (reported) { return; }
    reported = 1;

    // A lost increment means the spinlock is not actually mutually
    // exclusive across CPUs.
    if (par_counter != (uint64_t)PAR_THREADS * PAR_ITERATIONS) {
        serial_write_string("[smp] parallel selftest FAILED: counter lost increments\n");
        return;
    }
    int distinct = 0;
    for (int i = 0; i < MAX_CPUS; i++) { if (par_cpu_seen[i]) { distinct++; } }
    // The assertion that fails on a single-CPU kernel: work must have
    // actually executed on more than one CPU.
    if (distinct < 2) {
        serial_write_string("[smp] parallel selftest FAILED: work ran on only one cpu\n");
        return;
    }
    serial_write_string("[smp] parallel selftest passed, cpus=");
    serial_write_hex64((uint64_t)distinct);
    serial_write_string("\n");
}
