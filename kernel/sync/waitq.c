#include "sync/waitq.h"
#include "sync/lock.h"
#include "sched/proc.h"
#include "arch/cpu_local.h"
#include "errno.h"
#include "ipc/signal.h"
#include "drivers/char/timer.h"
#include "drivers/char/serial.h"
#include "smp/smp.h"

static struct waitq poll_broadcast;   // see the poll/select section below

// CS3 baseline for CS5.2. Every readiness change anywhere wakes EVERY
// poll sleeper, so wakeups grow as O(sleepers x events) where a
// per-object design would be O(interested). Counting both halves turns
// "the broadcast is a scaling concern" -- which sys_poll.c's own header
// has said for a while -- into a ratio a redesign has to move.
static volatile uint64_t poll_events;    // broadcasts issued
static volatile uint64_t poll_wakeups;   // sleepers actually woken

void waitq_poll_stats(uint64_t *events, uint64_t *wakeups) {
    if (events)  { *events  = poll_events; }
    if (wakeups) { *wakeups = poll_wakeups; }
}

void waitq_init(struct waitq *q) {
    q->head = 0;
    q->tail = 0;
    spin_init(&q->lock, LOCK_RANK_WAITQ, "waitq");
}

void waitq_global_init(void) {
    waitq_init(&poll_broadcast);
}

void waitq_lock_selftest(void) {
    struct waitq q;
    waitq_init(&q);
    if (q.lock.rank != LOCK_RANK_WAITQ) {
        serial_write_string("[waitq] selftest FAILED: lock rank wrong\n");
        return;
    }
    // A mutex passes its own guard into waitq_sleep as `release`, so
    // WAITQ must be acquirable while a guard of mutex rank is held.
    struct spinlock guard;
    spin_init(&guard, LOCK_RANK_VNODE, "selftest-guard");
    uint64_t f = spin_lock_irqsave(&guard);
    int ok = lock_rank_ok(LOCK_RANK_WAITQ);
    spin_unlock_irqrestore(&guard, f);
    if (!ok) {
        serial_write_string("[waitq] selftest FAILED: waitq not acquirable under a mutex guard\n");
        return;
    }
    // And RUNQUEUE must be acquirable under WAITQ, since the sleep path
    // reaches schedule() after touching the queue.
    uint64_t f2 = spin_lock_irqsave(&q.lock);
    int rq_ok = lock_rank_ok(LOCK_RANK_RUNQUEUE);
    spin_unlock_irqrestore(&q.lock, f2);
    if (!rq_ok) {
        serial_write_string("[waitq] selftest FAILED: runqueue not acquirable under waitq\n");
        return;
    }
    serial_write_string("[waitq] lock selftest passed\n");
}

static void waitq_make_ready(struct thread *t);

static void waitq_enqueue(struct waitq *q, struct thread *t) {
    t->next = 0;
    if (q->tail) { q->tail->next = t; } else { q->head = t; }
    q->tail = t;
}

static struct thread *waitq_dequeue(struct waitq *q) {
    struct thread *t = q->head;
    if (t) {
        q->head = t->next;
        if (!q->head) { q->tail = 0; }
        t->next = 0;
    }
    return t;
}

void waitq_remove(struct thread *t) {
    struct waitq *q = t->blocked_on;
    if (!q) { return; }
    uint64_t rf = spin_lock_irqsave(&q->lock);
    // Re-read under the lock: the thread may have been woken between the
    // read above and the acquire, in which case q is stale.
    if (t->blocked_on != q) {
        spin_unlock_irqrestore(&q->lock, rf);
        return;
    }
    struct thread **pp = &q->head;
    struct thread *prev = 0;
    while (*pp && *pp != t) { prev = *pp; pp = &(*pp)->next; }
    if (*pp) {
        *pp = t->next;
        // Repairing tail matters even when t is the ONLY queued thread:
        // leaving a stale tail pointing at a freed thread makes the
        // next enqueue write through it.
        if (q->tail == t) { q->tail = prev; }
    }
    t->next = 0;
    t->blocked_on = 0;
    spin_unlock_irqrestore(&q->lock, rf);
}

int waitq_sleep(struct waitq *q, struct spinlock *release) {
    struct thread *t = current_thread();

    // If the caller holds `release`, spin_lock_irqsave already cleared
    // IF. Otherwise clear it here. Either way nothing can wake us
    // between the enqueue and the schedule() below.
    uint64_t own_flags = 0;
    if (!release) {
        __asm__ volatile ("pushfq; pop %0; cli" : "=r"(own_flags) :: "memory");
    }

    if (signal_pending_any(t)) {
        if (!release && (own_flags & (1ULL << 9))) { __asm__ volatile ("sti"); }
        return -EINTR;
    }

    // The queue lock covers only the enqueue. It is released before
    // schedule(), which must be entered holding no spinlock at all --
    // schedule() panics otherwise.
    uint64_t qf = spin_lock_irqsave(&q->lock);
    waitq_enqueue(q, t);
    t->blocked_on = q;
    t->state      = THREAD_BLOCKED;
    spin_unlock_irqrestore(&q->lock, qf);

    if (release) { spin_unlock_irqrestore(release, 0); } // deliberately keeps IF off

    schedule();

    // Resumed: woken normally, or killed while blocked.
    t->blocked_on = 0;
    int rc = signal_pending_any(t) ? -EINTR : 0;

    if (release) { (void)spin_lock_irqsave(release); }
    else if (own_flags & (1ULL << 9)) { __asm__ volatile ("sti"); }
    return rc;
}

// Threads sleeping with a deadline, scanned once per timer tick.
static struct thread *timeout_list;
static struct spinlock timeout_lock;
static int timeout_lock_ready;

static void timeout_add(struct thread *t, uint64_t deadline) {
    if (!timeout_lock_ready) {
        spin_init(&timeout_lock, LOCK_RANK_TIMEOUT, "waitq-timeout");
        timeout_lock_ready = 1;
    }
    uint64_t f = spin_lock_irqsave(&timeout_lock);
    t->sleep_deadline = deadline;
    t->timeout_next = timeout_list;
    timeout_list = t;
    spin_unlock_irqrestore(&timeout_lock, f);
}

static void timeout_remove(struct thread *t) {
    if (!timeout_lock_ready) { return; }
    uint64_t f = spin_lock_irqsave(&timeout_lock);
    struct thread **pp = &timeout_list;
    while (*pp && *pp != t) { pp = &(*pp)->timeout_next; }
    if (*pp) { *pp = t->timeout_next; }
    t->timeout_next = 0;
    t->sleep_deadline = 0;
    spin_unlock_irqrestore(&timeout_lock, f);
}

void waitq_timeout_tick(void) {
    if (!timeout_lock_ready || !timeout_list) { return; }
    uint64_t now = timer_ticks();

    // Expired sleepers are unlinked under the lock and woken after it is
    // dropped. Waking can spin (thread_enqueue_ready waits for the
    // sleeper to finish leaving its CPU), and spinning while holding
    // timeout_lock would block every other CPU's timeout_add for the
    // duration -- for no reason, since the wake needs nothing this list
    // protects.
    struct thread *expired = 0;

    uint64_t f = spin_lock_irqsave(&timeout_lock);
    struct thread **pp = &timeout_list;
    while (*pp) {
        struct thread *t = *pp;
        if (t->sleep_deadline && now >= t->sleep_deadline) {
            *pp = t->timeout_next;
            t->sleep_deadline = 0;
            t->timeout_next = expired;
            expired = t;
        } else {
            pp = &t->timeout_next;
        }
    }
    spin_unlock_irqrestore(&timeout_lock, f);

    while (expired) {
        struct thread *t = expired;
        expired = t->timeout_next;
        t->timeout_next = 0;
        if (t->state == THREAD_BLOCKED) {
            waitq_remove(t);
            waitq_make_ready(t);
        }
    }
}

int waitq_sleep_timeout(struct waitq *q, struct spinlock *release,
                        uint64_t deadline) {
    struct thread *t = current_thread();
    if (timer_ticks() >= deadline) { return -ETIMEDOUT; }
    timeout_add(t, deadline);
    int rc = waitq_sleep(q, release);
    int expired = (t->sleep_deadline == 0 && rc == 0);
    timeout_remove(t);
    if (rc != 0) { return rc; }
    return expired ? -ETIMEDOUT : 0;
}

// thread_wake does the BLOCKED -> READY transition atomically, so a
// sleeper that two wakers reach at once (waitq_wake_one racing a
// SIGKILL, say) is enqueued exactly once. Its return value is ignored
// here: losing the race means someone else already woke the thread,
// which is the outcome either way.
static void waitq_make_ready(struct thread *t) {
    thread_wake(t, THREAD_BLOCKED);
}

// Both wakers dequeue under q->lock and only then make the threads
// ready: thread_enqueue_ready takes a run queue lock (rank 10), and
// keeping the waitq lock (rank 9) held across it would be legal but
// would hold two locks for no reason.
void waitq_wake_one(struct waitq *q) {
    uint64_t f = spin_lock_irqsave(&q->lock);
    struct thread *t = waitq_dequeue(q);
    spin_unlock_irqrestore(&q->lock, f);
    if (t) { waitq_make_ready(t); }
    if (waitq_poll_active && q != &poll_broadcast) { waitq_poll_notify(); }
}

void waitq_wake_all(struct waitq *q) {
    // Drain into a local list first, so the queue lock is dropped before
    // any thread is enqueued on a run queue.
    uint64_t f = spin_lock_irqsave(&q->lock);
    struct thread *woken = 0;
    struct thread *t;
    while ((t = waitq_dequeue(q)) != 0) {
        t->next = woken;
        woken = t;
    }
    spin_unlock_irqrestore(&q->lock, f);

    while (woken) {
        struct thread *next = woken->next;
        woken->next = 0;
        waitq_make_ready(woken);
        woken = next;
    }

    // A readiness change on any object may satisfy a poll/select.
    if (waitq_poll_active && q != &poll_broadcast) { waitq_poll_notify(); }
}

// ---- poll/select broadcast ----------------------------------------

volatile int waitq_poll_active;

void waitq_poll_notify(void) {
    __atomic_add_fetch(&poll_events, 1, __ATOMIC_RELAXED);
    // Count the sleepers this one broadcast is about to wake. Reading
    // the queue length without its lock is fine for a statistic: the
    // number is advisory, and taking the lock here would nest inside
    // waitq_wake_all's own acquire.
    for (struct thread *t = poll_broadcast.head; t; t = t->next) {
        __atomic_add_fetch(&poll_wakeups, 1, __ATOMIC_RELAXED);
    }
    waitq_wake_all(&poll_broadcast);
}

void waitq_poll_enter(void) { __atomic_add_fetch(&waitq_poll_active, 1, __ATOMIC_ACQ_REL); }
void waitq_poll_leave(void) { __atomic_sub_fetch(&waitq_poll_active, 1, __ATOMIC_ACQ_REL); }

int waitq_poll_wait(uint64_t deadline) {
    // Poll waiters never hold a lock across this, so pass none.
    return waitq_sleep_timeout(&poll_broadcast, 0, deadline);
}

// ---------------------------------------------------------------- selftest

static struct waitq  selftest_q;
static volatile int  selftest_stage;

static void selftest_sleeper(void) {
    selftest_stage = 1;
    waitq_sleep(&selftest_q, 0);
    selftest_stage = 2;
    thread_exit_self(0);
}

// Runs as a kernel thread rather than inline in kmain: it needs to
// schedule() and come back, which only works for a real thread (kmain
// has no saved context -- schedule() abandons its stack).
static void selftest_thread(void) {
    waitq_init(&selftest_q);
    selftest_stage = 0;

    struct thread *t = thread_alloc_kernel(selftest_sleeper);
    if (!t) {
        serial_write_string("[waitq] selftest FAILED: thread_alloc_kernel\n");
        thread_exit_self(1);
    }

    while (selftest_stage == 0) { schedule(); } // let it run until it blocks

    if (selftest_q.head != t) {
        serial_write_string("[waitq] selftest FAILED: sleeper not queued\n");
        thread_exit_self(1);
    }
    if (t->state != THREAD_BLOCKED) {
        serial_write_string("[waitq] selftest FAILED: sleeper not BLOCKED\n");
        thread_exit_self(1);
    }

    waitq_wake_one(&selftest_q);
    if (selftest_q.head != 0 || selftest_q.tail != 0) {
        serial_write_string("[waitq] selftest FAILED: queue not empty after wake\n");
        thread_exit_self(1);
    }

    while (selftest_stage != 2) { schedule(); }

    // Deliberately frees NOTHING here. The old code did, on the claim
    // that the sleeper "is a ZOMBIE now and not running" -- but
    // selftest_stage is set to 2 BEFORE thread_exit_self, so at this
    // point the sleeper is still executing its own exit path, and
    // still on the very kernel stack this used to hand back to the
    // page allocator. A timer interrupt in that window is enough to
    // run us here, even on one CPU.
    //
    // Worse, idle_entry's kzombies drain frees the same thread again,
    // so one heap slot landed on the free list twice and a later
    // kmalloc handed the same memory to two owners. (Diagnosed from a
    // free_list pointer of 0xfffffffc -- -4, the idle tid for CPU 3.)
    //
    // Kernel threads are owned by idle_entry's drain. Leave them to it.

    serial_write_string("[waitq] selftest passed\n");
    thread_exit_self(0);
}

void waitq_selftest_start(void) {
    thread_alloc_kernel(selftest_thread);
}

// CS2.4: the exit-vs-drain race, forced rather than waited for.
//
// A thread that frees itself is still running on its own kernel stack
// when thread_exit_self publishes it to kzombies, so idle_entry's drain
// on another CPU can reach it mid-exit. That was diagnosed once from a
// free_list pointer of 0xfffffffc and fixed (thread_wait_off_cpu plus
// refcounting), but nothing forced the window to reopen -- it happened
// by luck, under one timer/SMP interleaving.
//
// Hundreds of short-lived threads spread across every online CPU make
// the window likely instead of lucky. Under DEBUG_HEAP a second free of
// the same slot panics immediately, naming it, instead of corrupting
// the free list for some later owner.
#define CHURN_ROUNDS 400

static volatile int churn_live;

static void churn_thread(void) {
    __atomic_fetch_sub(&churn_live, 1, __ATOMIC_RELEASE);
    thread_exit_self(0);
}

void waitq_churn_selftest(void) {
    int online = smp_online_count();
    if (online < 2) {
        serial_write_string("[waitq] churn SKIPPED: single CPU\n");
        return;
    }
    churn_live = 0;
    for (int i = 0; i < CHURN_ROUNDS; i++) {
        __atomic_fetch_add(&churn_live, 1, __ATOMIC_ACQUIRE);
        if (!thread_alloc_kernel_on(churn_thread, i % online)) {
            __atomic_fetch_sub(&churn_live, 1, __ATOMIC_RELEASE);
            serial_write_string("[waitq] churn SKIPPED: thread_alloc failed\n");
            return;
        }
    }
    // Bounded: a double free panics before we get here under DEBUG_HEAP,
    // and a lost thread must not hang the boot.
    for (uint64_t spins = 0; churn_live > 0 && spins < 4000000000ULL; spins++) {
        __asm__ volatile ("pause");
    }
    if (churn_live > 0) {
        serial_write_string("[waitq] churn FAILED: threads never exited\n");
        return;
    }
    serial_write_string("[waitq] churn passed\n");
}
