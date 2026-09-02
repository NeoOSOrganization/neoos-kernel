// kernel/smp_selftest.c -- selftests that only mean anything with more
// than one CPU. Kept out of smp.c so the bringup code stays readable.

#include "smp/smp.h"
#include "arch/cpu_local.h"
#include "sync/lock.h"
#include "drivers/char/serial.h"
#include "sched/proc.h"
#include "sched/sched.h"
#include "arch/tss.h"
#include "arch/gdt.h"

// Every CPU must be taking its OWN local timer interrupt. Without that
// an AP never preempts anything: a compute-bound thread that reached one
// would run to completion, so there would be no point migrating work
// there. The LAPIC timer is a per-CPU device and was armed only on the
// BSP until timer_init_this_cpu existed -- a gap invisible from the
// serial log, since the BSP kept ticking exactly as before.
// Polled from the idle loop rather than run inline in kmain: the BSP
// reaches kmain's selftests with interrupts still disabled, so it could
// not observe its own timer there, and enabling them would let the first
// tick schedule() away from kmain's stack for good.
//
// Reports once, and only on success. A CPU whose timer never fires
// therefore produces NO line at all -- which is why the pass marker is
// in the Makefile's REQUIRED_MARKERS, where a missing suite is a
// failure rather than a silence.
void smp_timer_selftest_check(void) {
    static int reported;
    if (reported) { return; }

    int online = smp_online_count();
    for (int i = 0; i < online; i++) {
        if (__atomic_load_n(&cpus[i].timer_ticks_local, __ATOMIC_ACQUIRE) == 0) {
            return;
        }
    }
    reported = 1;
    serial_write_string("[smp] local timer selftest passed, cpus=");
    serial_write_hex64((uint64_t)online);
    serial_write_string("\n");
}

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
        // Placed directly rather than queued-then-moved: a thread that
        // is briefly visible on CPU 0's queue can be picked up, run to
        // completion and freed before the move lands.
        if (!thread_alloc_kernel_on(par_worker, i % online)) { return; }
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

// ---- work stealing ---------------------------------------------------
//
// The assertion is the STEAL COUNTER, not where these particular
// workers happened to run.
//
// The obvious test -- queue 16 threads on CPU 0, assert two CPUs ran
// one -- was tried first and failed about one boot in four. Whether a
// steal is observed in a short window depends on whether some other CPU
// happens to be idle at that moment, and with eleven processes and a
// dozen kernel threads running, often none is: steal_work only looks
// when a CPU's own queue is empty. That is correct behaviour being
// reported as a failure, which is the worst kind of test.
//
// So the workers still prove the path is exercised, and the pass line
// still reports how many CPUs took part -- but what makes it pass or
// fail is whether any CPU has stolen anything at all since boot.

#define STEAL_THREADS 16

static volatile int     steal_done;
static volatile uint8_t steal_cpu_seen[MAX_CPUS];

static void steal_worker(void) {
    steal_cpu_seen[this_cpu() - &cpus[0]] = 1;
    // A little work, so a stealing CPU has a window in which to observe
    // a non-empty victim queue rather than everything draining on CPU 0
    // before any other CPU looks.
    for (volatile int i = 0; i < 200000; i++) { }
    steal_cpu_seen[this_cpu() - &cpus[0]] = 1;   // and where it FINISHED
    __atomic_fetch_add((int *)&steal_done, 1, __ATOMIC_ACQ_REL);
    thread_exit_self(0);
}

void smp_steal_selftest_start(void) {
    steal_done = 0;
    for (int i = 0; i < MAX_CPUS; i++) { steal_cpu_seen[i] = 0; }
    for (int i = 0; i < STEAL_THREADS; i++) {
        if (!thread_alloc_kernel_on(steal_worker, 0)) { return; }
    }

    // The pass condition is "a USER thread migrated since boot". Since
    // /SBIN/INIT became PID 1 the kernel no longer floods CPU 0 with
    // user processes at boot, so give this test its own migratable
    // user workload: a handful of short-lived LOOPERs, all enqueued on
    // CPU 0 with idle APs ready to steal them. They are parented to no
    // one (spawned from the boot path) and exit on their own; the
    // struct each leaves behind is reclaimed at power-off.
#ifndef NEOOS_QUIET_BOOT
    for (int i = 0; i < 6; i++) { spawn("/BIN/LOOPER.ELF"); }
#else
    // Quiet boot: these six processes print "[looper] tick" for as long
    // as they live, and an interactive session is not the place for
    // them. The migration check below then has no workload and reports
    // nothing, which is correct -- a boot that runs no selftests should
    // not claim to have passed one.
#endif
}

void smp_steal_selftest_check(void) {
    if (__atomic_load_n((int *)&steal_done, __ATOMIC_ACQUIRE) != STEAL_THREADS) {
        return;
    }
    static int reported;
    if (reported) { return; }
    reported = 1;

    int online = smp_online_count();
    int seen = 0;
    uint64_t steals = 0, user = 0;
    for (int i = 0; i < online; i++) {
        if (steal_cpu_seen[i]) { seen++; }
        steals += __atomic_load_n(&cpus[i].steals, __ATOMIC_ACQUIRE);
        user   += __atomic_load_n(&cpus[i].steals_user, __ATOMIC_ACQUIRE);
    }
    if (steals == 0) {
        serial_write_string("[smp] steal selftest FAILED: no cpu ever stole any work\n");
        return;
    }
    // The claim this milestone actually made -- that a thread carrying
    // an address space, an fd table and signal state is safe to move --
    // is asserted here rather than inferred from a total that kernel
    // threads alone could account for.
    if (user == 0) {
        serial_write_string("[smp] steal selftest FAILED: no USER thread ever migrated\n");
        return;
    }
    serial_write_string("[smp] steal selftest passed, steals=");
    serial_write_hex64(steals);
    serial_write_string(" user=");
    serial_write_hex64(user);
    serial_write_string(" worker cpus=");
    serial_write_hex64((uint64_t)seen);
    serial_write_string("\n");
}

void panic_stop_selftest(void) {
    // Cannot be tested by triggering it -- a real panic stops the boot --
    // so the mechanism's preconditions are asserted instead.
    // The stop is delivered by NMI (vector 2), NOT by VECTOR_IPI_PANIC
    // -- nothing has ever sent that constant. Checking it was the whole
    // reason this suite reported green while the mechanism was broken,
    // so the NMI is actually fired and observed instead.
    if (!smp_nmi_selftest()) {
        serial_write_string("[smp] panic-stop selftest FAILED: NMI never reached nmi_handler\n");
        return;
    }
    // The handler must not take the serial lock: a panicking CPU may be
    // holding it, so a handler that waited on it would hang the stop.
    if (smp_panic_handler_takes_serial_lock()) {
        serial_write_string("[smp] panic-stop selftest FAILED: handler touches the serial lock\n");
        return;
    }
    if (smp_online_count() < 2) {
        serial_write_string("[smp] panic-stop selftest FAILED: no other cpu to stop\n");
        return;
    }
    serial_write_string("[smp] panic-stop selftest passed\n");
}
