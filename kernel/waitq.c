#include "waitq.h"
#include "lock.h"
#include "sched/proc.h"
#include "cpu_local.h"
#include "mm/pmm.h"
#include "mm/heap.h"
#include "errno.h"
#include "signal.h"
#include "serial.h"

void waitq_init(struct waitq *q) { q->head = 0; q->tail = 0; }

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

    waitq_enqueue(q, t);
    t->blocked_on = q;
    t->state      = THREAD_BLOCKED;

    if (release) { spin_unlock_irqrestore(release, 0); } // deliberately keeps IF off

    schedule();

    // Resumed: woken normally, or killed while blocked.
    t->blocked_on = 0;
    int rc = signal_pending_any(t) ? -EINTR : 0;

    if (release) { (void)spin_lock_irqsave(release); }
    else if (own_flags & (1ULL << 9)) { __asm__ volatile ("sti"); }
    return rc;
}

static void waitq_make_ready(struct thread *t) {
    t->blocked_on = 0;
    t->state = THREAD_READY;
    thread_enqueue_ready(t);
}

void waitq_wake_one(struct waitq *q) {
    struct thread *t = waitq_dequeue(q);
    if (t) { waitq_make_ready(t); }
}

void waitq_wake_all(struct waitq *q) {
    struct thread *t;
    while ((t = waitq_dequeue(q)) != 0) { waitq_make_ready(t); }
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

    // The sleeper is a ZOMBIE now and not running, so its stack can be
    // reclaimed here rather than left to the idle thread's drain.
    pmm_free(t->kernel_stack_phys, KERNEL_STACK_ORDER);
    kfree(t);

    serial_write_string("[waitq] selftest passed\n");
    thread_exit_self(0);
}

void waitq_selftest_start(void) {
    thread_alloc_kernel(selftest_thread);
}
