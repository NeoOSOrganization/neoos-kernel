// kernel/sched/sched.c -- run queue, context switching, and the idle
// thread. Split out of the former kernel/process.c; the code is
// unchanged, only relocated.

#include "sched/sched.h"
#include "mm/pmm.h"
#include "mm/paging.h"
#include "mm/heap.h"
#include "arch/tss.h"
#include "drivers/char/serial.h"
#include "fs/vfs.h"
#include "elf.h"
#include "arch/cpu.h"
#include "arch/cpu_local.h"
#include "arch/msr.h"
#include "sync/waitq.h"
#include "sync/lock.h"
#include "errno.h"
#include "smp/smp.h"
#include "drivers/char/timer.h"
#include "sched/rq.h"

extern void context_switch(uint64_t *old_rsp, uint64_t *new_rsp);
extern void kernel_thread_entry_trampoline(void);
extern void kernel_thread_trampoline(void);
extern void fork_trampoline(void);

// The per-CPU ready queue is the fair class's cfs_rq (an rbtree),
// reached as this_cpu()->rq.cfs. SCH-1 Task 1 keeps it FIFO-ordered
// (fair.c keys the tree by insertion order) so behaviour is unchanged;
// Tasks 2-4 turn it into real EEVDF.

struct rq *cpu_rq(int cpu_index) { return &cpus[cpu_index].rq; }
struct rq *this_rq(void)         { return &this_cpu()->rq; }

// The rq clock. SCH-1 Task 4 moved this from the 100 Hz tick counter
// (10 ms granularity -- far too coarse for a 0.7 ms base slice) to
// sched_clock_ns(), a calibrated rdtsc-based nanosecond clock. Steal-
// time / irq-time accounting (clock_task < clock) is still SCH-7.
void rq_clock_update(struct rq *rq) {
    uint64_t now = sched_clock_ns();
    rq->clock = now;
    rq->clock_task = now;
}

// Timer-tick entry points. timer_handler holds no rq lock; these take
// it, so fair.c stays "lock already held" throughout.
int sched_tick(struct rq *rq) {
    uint64_t f = spin_lock_irqsave(&rq->lock);
    rq_clock_update(rq);
    int resched = fair_entity_tick(rq);
    spin_unlock_irqrestore(&rq->lock, f);
    return resched;
}

uint64_t sched_slice_remaining_ns(struct rq *rq) {
    uint64_t f = spin_lock_irqsave(&rq->lock);
    rq_clock_update(rq);
    uint64_t ns = fair_slice_remaining_ns(rq);
    spin_unlock_irqrestore(&rq->lock, f);
    return ns;
}

// ---- scheduler ABI (SCH-1 Task 5) ----------------------------------

void sched_apply_attr(struct thread *t, int nice, int policy, uint64_t slice_ns) {
    if (nice < -20) { nice = -20; }
    if (nice >  19) { nice =  19; }
    struct rq *rq = this_rq();
    uint64_t f = spin_lock_irqsave(&rq->lock);
    rq_clock_update(rq);
    if (t == this_cpu()->current) {
        fair_reweight_current(rq, nice, policy, slice_ns);
    } else {
        // Not running here: record on the entity. set_load_weight() at
        // its next enqueue picks it up -- we must NOT change load.weight
        // now (it may be in another CPU's tree, whose avg_* sums were
        // built with the old weight). Documented in docs/stdlib.md.
        t->se.nice   = nice;
        t->se.policy = policy;
        if (slice_ns != (uint64_t)-1) { t->se.slice = slice_ns; }
    }
    t->se.batch_hint = (policy == SCHED_BATCH) ? 1 : 0;
    spin_unlock_irqrestore(&rq->lock, f);
}

void sched_do_yield(void) {
    struct rq *rq = this_rq();
    uint64_t f = spin_lock_irqsave(&rq->lock);
    rq_clock_update(rq);
    fair_yield_current(rq);
    spin_unlock_irqrestore(&rq->lock, f);
    schedule();
}

// Blocks until no CPU is executing on `t`'s kernel stack any more.
//
// THE INVARIANT: a thread must not be placed on a run queue while a CPU
// is still switching away from it. context_switch writes saved_rsp as
// its FIRST action, so a thread published before that point is visible
// with a stale saved_rsp -- and a second CPU that picks it up starts
// executing on a kernel stack the first CPU has not finished with. That
// corrupts the heap within milliseconds (observed as a page fault
// inside kmalloc at cr2=0x100000000).
//
// schedule() solves this for the thread it is switching away from by
// deferring the requeue to sched_post_switch(). But a thread can also
// be made runnable by somebody ELSE -- a waitq wake, a timeout, SIGCONT
// -- on another CPU entirely, while it is still on its way into
// schedule(). That waker cannot defer; it has to wait.
//
// The wait is bounded: the owning CPU only has to finish a context
// switch, and it holds no lock a caller here could be waiting on
// (schedule() panics if entered with any lock held). A wait that never
// ends therefore means a design error -- somebody is trying to make a
// RUNNING thread runnable -- so say that instead of hanging silently.
static void wait_off_cpu(struct thread *t) {
    for (uint64_t i = 0; i < 100000000ULL; i++) {
        if (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE) == 0) { return; }
        __asm__ volatile ("pause");
    }
    lock_panic("thread still on-cpu; cannot be made runnable", "sched/on_cpu", 0);
}

void enqueue_ready(struct thread *t) {
    wait_off_cpu(t);
    struct rq *rq = this_rq();
    uint64_t f = spin_lock_irqsave(&rq->lock);
    rq_clock_update(rq);
    fair_enqueue(rq, t);
    spin_unlock_irqrestore(&rq->lock, f);
}

// Retained for API compatibility; schedule() calls fair_pick directly.
struct thread *dequeue_ready(void) {
    struct rq *rq = this_rq();
    uint64_t f = spin_lock_irqsave(&rq->lock);
    rq_clock_update(rq);
    struct thread *t = fair_pick(rq);
    spin_unlock_irqrestore(&rq->lock, f);
    return t;
}

void thread_enqueue_ready(struct thread *t) { enqueue_ready(t); }

// The one way to make a parked thread runnable. Returns 1 if THIS
// caller performed the transition.
//
// Two things have to be true at once, and neither survives on its own:
//
//   - the thread must have finished leaving its CPU (wait_off_cpu),
//     or it lands on a run queue with a stale saved_rsp;
//   - exactly ONE waker may enqueue it. Several can race for the same
//     sleeper -- waitq_wake_one and a SIGKILL arriving together, say --
//     and before this was a compare-exchange both would "succeed",
//     putting one thread on two run queues, after which two CPUs run
//     it on one kernel stack.
//
// The CAS is what makes the pairing safe: no waker may touch `state`
// until on_cpu is clear, so schedule()'s own hand-off in
// sched_post_switch() reads a state no waker has raced it for, and
// among the wakers themselves only the winner enqueues.
int thread_wake(struct thread *t, enum thread_state from) {
    // Checked BEFORE the wait, not just by the CAS. A thread that is
    // still RUNNING is not parked and never will be on our account, so
    // wait_off_cpu() would spin on it until it gave up and panicked.
    // Once the state does read `from`, the thread is committed to
    // leaving its CPU and the wait is short.
    if (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != from) { return 0; }
    wait_off_cpu(t);
    enum thread_state expected = from;
    if (!__atomic_compare_exchange_n(&t->state, &expected, THREAD_READY,
                                     0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return 0;
    }
    t->blocked_on = 0;
    enqueue_ready(t);
    return 1;
}

void thread_wait_off_cpu(struct thread *t) { wait_off_cpu(t); }

// Enqueue onto a SPECIFIC CPU's queue rather than this one. Used at
// thread creation to spread new work, and by the selftests.
void enqueue_ready_on(int cpu_index, struct thread *t) {
    wait_off_cpu(t);
    struct rq *rq = cpu_rq(cpu_index);
    uint64_t f = spin_lock_irqsave(&rq->lock);
    rq_clock_update(rq);
    fair_enqueue(rq, t);
    spin_unlock_irqrestore(&rq->lock, f);
    // Sent AFTER the unlock: the target may be spinning on this very lock
    // with interrupts disabled and could not take the IPI. Without this
    // poke a target parked in idle's `sti; hlt` waits for its next local
    // timer tick before noticing the work.
    smp_send_reschedule(cpu_index);
}

// Takes one thread from the busiest remote run queue. Called only when
// this CPU's own queue is empty, so it never makes a CPU busier at
// another's expense; it only stops a core idling while work is stacked
// up elsewhere.
//
// Two run queue locks are held at once and both are LOCK_RANK_RUNQUEUE
// -- equal ranks, which the checker forbids for good reason. Safety
// comes from a consistent global order instead: spin_lock_ordered_pair
// always acquires in ADDRESS order, and the queue locks live inside
// cpus[], so address order is CPU-index order. No two stealing CPUs can
// build a cycle.
//
// Any thread may be stolen, kernel or user. That is a deliberate change
// from the first attempt, which took kernel threads only, on the theory
// that a user thread's address space, fd table and signal state carried
// single-CPU assumptions. They did -- but the assumptions were in the
// scheduler, not in the process code: the SYSCALL MSRs were programmed
// on the BSP alone, threads were published to run queues before their
// context was saved, reapers freed stacks that were still in use, and
// SIGCONT could be lost against a stop in flight. Those are fixed and
// asserted now, and the per-CPU state a user thread actually needs
// (CR3, TSS.rsp0, the xstate area, GS) is already reloaded on every
// switch.
//
// An idle CPU discovers new work on its next local timer tick rather
// than being poked: 10ms of latency in exchange for not sending an IPI
// on every enqueue.
static struct thread *steal_work(struct cpu *self) {
    int online = smp_online_count();
    if (online < 2) { return 0; }

    // The victim is chosen by count WITHOUT locking. A stale count only
    // costs a wasted attempt, never a corrupt list -- the real check
    // happens under the lock below.
    struct cpu *victim = 0;
    uint32_t best = 0;
    for (int i = 0; i < online; i++) {
        struct cpu *c = &cpus[i];
        if (c == self) { continue; }
        uint32_t n = __atomic_load_n(&c->rq.cfs.nr_running, __ATOMIC_RELAXED);
        if (n > best) { best = n; victim = c; }
    }
    if (!victim) { return 0; }

    uint64_t f = spin_lock_ordered_pair(&self->rq.lock, &victim->rq.lock);
    // Re-checked under the lock: the victim may have been drained
    // between the scan and the acquire.
    struct thread *t = victim->rq.cfs.nr_running > 0 ? fair_steal(&victim->rq) : 0;
    if (t) {
        self->steals++;                     // under the lock; only this CPU writes it
        if (t->proc) { self->steals_user++; }
    }
    spin_unlock_ordered_pair(&self->rq.lock, &victim->rq.lock, f);
    return t;
}

// dequeue_specific() lived here. It existed only to undo an enqueue
// that should never have happened -- "allocate onto this CPU's queue,
// then take it back off" -- and it only ever searched this_cpu()'s
// queue, so it could not have found a thread another CPU had already
// taken. thread_alloc_kernel_unqueued and thread_alloc_kernel_on
// replace both of its callers by placing the thread correctly to begin
// with. Deleted rather than fixed: nothing should want it.

// Runs whenever no other thread is ready. Having a real idle thread
// removes schedule()'s old "nothing ready, keep running whatever's
// current" special case for the blocked/dead-current cases.
// Kernel threads (proc == 0) have no parent to reap them and cannot
// free the stack they are running on, so they park here and the idle
// thread reclaims them. Without this every selftest thread would leak
// its 16KiB kernel stack for the life of the boot.
struct thread   *kzombies;
struct spinlock  kzombies_lock;

static void idle_entry(void) {
    for (;;) {
        uint64_t f = spin_lock_irqsave(&kzombies_lock);
        struct thread *z = kzombies;
        kzombies = 0;
        spin_unlock_irqrestore(&kzombies_lock, f);
        while (z) {
            struct thread *next = z->proc_next;
            // thread_exit_self publishes a thread to kzombies BEFORE it
            // reaches schedule(), so it is advertised as reapable while
            // still executing its own exit path on this very stack.
            // Safe on one CPU by construction -- no reaper could run
            // until it had switched away -- and a use-after-free on
            // four. Wait for it to actually leave.
            thread_wait_off_cpu(z);
            pmm_free(z->kernel_stack_phys, KERNEL_STACK_ORDER);
            thread_put(z);   // the kzombies list's reference
            z = next;
        }
        smp_parallel_selftest_check();
        smp_timer_selftest_check();
        smp_steal_selftest_check();

        // The idle thread schedules for ITSELF rather than relying on
        // being preempted. Every CPU has a local timer now, so this is
        // no longer load-bearing the way it was when only the BSP was
        // preempted -- but an AP woken by a reschedule IPI would
        // otherwise loop and halt again, waiting up to a full tick to
        // pick up the work the IPI was announcing.
        schedule();

        __asm__ volatile ("sti; hlt");
    }
}

// One idle thread per CPU. Still called by each CPU FOR ITSELF, which
// is now a matter of taste rather than of correctness: the thread is
// never queued at all, so nothing here depends on this_cpu().
//
// The reserved tid is -(index+1) rather than 0 so idle threads stay
// distinguishable from each other -- and from real threads -- in the
// serial log.
void idle_init_for(int cpu_index) {
    // Never queued in the first place. This used to allocate it onto
    // this CPU's queue and then call dequeue_specific to take it back
    // off, which leaves a window in which another CPU can pick the idle
    // thread up and run it -- after which cpus[i].idle names a thread
    // executing on a different CPU, and schedule()'s fallback to it
    // puts two CPUs on one kernel stack.
    struct thread *t = thread_alloc_kernel_unqueued(idle_entry);
    t->tid = -(cpu_index + 1);
    cpus[cpu_index].idle = t;
}

void idle_init(void) { idle_init_for(0); }

// Releases the thread this CPU switched away from, and only then makes
// it runnable. Runs as the INCOMING thread, which is the earliest point
// at which the outgoing thread's context is definitely saved and its
// kernel stack definitely idle.
//
// Called from three kinds of place, which between them cover every way
// a CPU can arrive on a new thread:
//   - the top of schedule(), for a thread resumed by an earlier switch;
//   - immediately after context_switch() returns, for the common case;
//   - the head of each trampoline, for a brand-new thread, which starts
//     at its trampoline and never reaches the post-switch path.
//
// The order inside matters. on_cpu is cleared BEFORE the requeue: clear
// it afterwards and a CPU that picks the thread up in between would set
// on_cpu itself, only for this stale store to clear it again while that
// CPU is running the thread. Reading `s` before the clear is safe
// because nothing else may touch a thread's state while on_cpu is set.
void sched_post_switch(void) {
    struct cpu *c = this_cpu();
    struct thread *p = c->rq.prev_pending;
    if (!p) { return; }
    c->rq.prev_pending = 0;

    enum thread_state s = p->state;
    __atomic_store_n(&p->on_cpu, 0, __ATOMIC_RELEASE);

    // The idle thread is this CPU's alone and is never queued anywhere;
    // schedule() falls back to c->idle instead of dequeuing it.
    if (p == c->idle) { return; }

    if (s == THREAD_READY) {
        // Preempted, not blocked -- schedule() left its sched_entity out
        // of the tree (neither curr nor queued). Put it back keeping its
        // vruntime, now that its context is definitely saved.
        struct rq *rq = &c->rq;
        uint64_t f = spin_lock_irqsave(&rq->lock);
        fair_requeue_preempted(rq, p);
        spin_unlock_irqrestore(&rq->lock, f);
    }
    // THREAD_BLOCKED: fair_block_current() already removed it in
    // schedule(); its waker re-adds it via enqueue_ready() once on_cpu
    // is clear. THREAD_ZOMBIE: published by thread_exit_self; when it
    // reached schedule() its state was already != RUNNING so
    // fair_block_current() took it out. Nothing to do for either.
}

// Restores EFLAGS.IF to whatever it was on entry to schedule(). Split
// out because schedule() has three exits (no-task, same-task, and the
// far side of a context switch, possibly milliseconds later in a
// different task).
static inline void schedule_restore_if(uint64_t saved_flags) {
    if (saved_flags & (1ULL << 9)) {
        __asm__ volatile ("sti");
    }
}

void schedule(void) {
    // Entering schedule() with a spinlock held deadlocks every other CPU
    // the moment SMP is real: the lock is released only when this thread
    // runs again, and this thread runs again only when some other CPU
    // makes progress. On one CPU it silently "works", which is exactly
    // why it needs an assertion rather than a comment. waitq_sleep
    // already releases the caller's guard before calling us; this makes
    // that an enforced invariant rather than an accident.
    if (lock_held_depth() != 0) {
        lock_panic("schedule() with a spinlock held", "schedule",
                   lock_held_top_name());
    }

    // schedule() is NOT reentrant, and until this cli it ran with
    // interrupts enabled. Between `current = next` and the
    // context_switch() below, `current` already names the incoming
    // task while execution is still on the OUTGOING task's stack -- so
    // a timer interrupt landing in that window re-enters schedule()
    // with prev == the incoming task, and context_switch's
    // `mov [rdi], rsp` stamps the outgoing task's RSP into the
    // incoming task's saved_rsp. That task is then resumed on a stack
    // that isn't its own (observed: pid 6 resumed with an RSP pointing
    // into pid 5's kernel stack, faulting in syscall_dispatch's
    // epilogue with a garbage RBP, escalating to a double fault).
    //
    // The window was always there, but nothing hit it until fork()
    // made it easy to have several tasks doing nothing but yield(),
    // which keeps schedule() executing a large fraction of the time.
    //
    // IF is restored rather than unconditionally set because
    // timer_handler() calls schedule() from an interrupt gate with
    // IF already 0, and must return to the ISR with it still 0 -- the
    // iretq there is what re-enables it. `flags` is a local, so it
    // lives on this task's own kernel stack and is still correct
    // whenever this task is eventually resumed.
    uint64_t flags;
    __asm__ volatile ("pushfq; pop %0; cli" : "=r"(flags) :: "memory");

    struct cpu *c = this_cpu();

    // Before looking for work: whoever this CPU switched away from last
    // is now fully saved, and may be handed on.
    sched_post_switch();

    struct thread *cur0 = c->current;
    struct rq *rq = &c->rq;

    // EEVDF pick, under rq->lock, with the rq clock current. Released
    // before any context switch (a spinlock must never span one).
    uint64_t rf = spin_lock_irqsave(&rq->lock);
    rq_clock_update(rq);
    // A prev that is no longer RUNNING is blocking or exiting -- take it
    // out of the fair class now (Linux's deactivate_task).
    if (cur0 && cur0->state != THREAD_RUNNING && rq->cfs.curr == &cur0->se) {
        fair_block_current(rq, cur0);
    }
    struct thread *next = fair_pick(rq);
    spin_unlock_irqrestore(&rq->lock, rf);

    if (!next) {
        struct thread *stolen = steal_work(c);
        if (stolen) {
            rf = spin_lock_irqsave(&rq->lock);
            fair_accept_stolen(rq, stolen);
            next = fair_pick(rq);
            spin_unlock_irqrestore(&rq->lock, rf);
        }
    }
    if (!next) {
        struct thread *cur = c->current;
        if (cur && cur->state == THREAD_RUNNING) {
            schedule_restore_if(flags);
            return; // nothing else ready; keep running whatever's current
        }
        next = c->idle; // current is blocked or dead -- park on idle
    }

    struct thread *prev = c->current;

    // The invariant wait_off_cpu() protects, asserted from the other
    // side: a runnable thread must not still be executing anywhere. If
    // this ever fires, something published a thread before its context
    // was saved -- exactly the bug that made work stealing corrupt the
    // heap. Far better to name it here than to discover it as a
    // mangled free list ten milliseconds later.
    if (next != prev && __atomic_load_n(&next->on_cpu, __ATOMIC_ACQUIRE)) {
        lock_panic("scheduling a thread that is still on another cpu",
                   "sched/on_cpu", 0);
    }

    next->state = THREAD_RUNNING;
    __atomic_store_n(&next->on_cpu, 1, __ATOMIC_RELAXED);
    c->current = next;

    // The thread pointer is per-THREAD but the MSR is per-CPU, so it
    // has to be reloaded on every switch -- otherwise a thread arriving
    // on a CPU keeps whatever FS base the previous occupant left, and
    // every one of its __thread variables reads another thread's
    // storage. Migration makes that a certainty rather than a
    // possibility.
    //
    // Cached per CPU because WRMSR is expensive and the common case is
    // that nothing changed: two kernel threads in a row, or one thread
    // preempted and resumed. Nothing else writes IA32_FS_BASE, so the
    // cache cannot go stale behind our back.
    if (next->fs_base != c->fs_base_loaded) {
        c->fs_base_loaded = next->fs_base;
        wrmsr(MSR_FS_BASE, next->fs_base);
    }
    c->tss->rsp0    = next->kernel_stack_top;
    c->kernel_stack = next->kernel_stack_top;

    // Always establish a definite CR3, even for a kernel-mode-only task
    // (pml4_phys == 0 -- falls back to the kernel's own never-freed
    // p4_table). Leaving CR3 unchanged in that case used to be harmless
    // (an exited process's now-zombie PML4 just leaked, unused-but-
    // intact memory), but now that task_exit() actually frees a
    // process's PML4 frame back to the allocator, a stale CR3 left
    // pointing at it could get silently reused and overwritten by the
    // very next pmm_alloc() -- corrupting the page table the CPU is
    // still actively translating through.
    uint64_t next_cr3 = (next->proc && next->proc->pml4_phys)
                      ? next->proc->pml4_phys
                      : (uint64_t)(uintptr_t)p4_table;
    __asm__ volatile ("mov %0, %%cr3" :: "r"(next_cr3) : "memory");

    if (prev == next) {
        schedule_restore_if(flags);
        return;
    }

    // Handed over ONLY once a context switch is certain. Setting this
    // before the `prev == next` early return above would leave a
    // still-RUNNING thread in prev_pending, and the next release would
    // put it on a run queue while it is executing -- precisely the race
    // this mechanism exists to prevent.
    if (prev) {
        if (prev->state == THREAD_RUNNING) { prev->state = THREAD_READY; }
        c->rq.prev_pending = prev;
    }

    static uint64_t discarded_rsp; // used the first time schedule() is ever called, from kmain
    if (prev) {
        cpu_state_save(prev->xstate);
    }
    cpu_state_restore(next->xstate);
    context_switch(prev ? &prev->saved_rsp : &discarded_rsp, &next->saved_rsp);

    // Reached only when THIS task is scheduled back in, which may be
    // much later and -- once threads migrate -- on a DIFFERENT CPU, so
    // sched_post_switch() re-reads this_cpu() rather than reusing `c`.
    sched_post_switch();

    // `flags` is the IF state from this task's own entry above.
    schedule_restore_if(flags);
}
