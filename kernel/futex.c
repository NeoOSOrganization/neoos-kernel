// kernel/futex.c -- Linux's futex, on NeoOS's wait queues.
//
// A futex is a user-space 32-bit word plus a kernel-side sleep queue
// that is consulted only when a thread actually has to block. The whole
// point is that the uncontended path never enters the kernel at all: a
// mutex lock is one compare-exchange in userland, and this code runs
// only when that fails. Every higher-level POSIX primitive -- mutexes,
// condition variables, semaphores, barriers, once-control, and read-
// write locks -- is built out of these two operations, which is why
// this is the one IPC primitive that had to come first.
//
// THE RACE THIS EXISTS TO CLOSE. A thread decides to sleep because it
// read some value from *uaddr; between that read and the sleep, another
// thread can change the value and issue the wake. Nothing would then
// wake the sleeper. FUTEX_WAIT therefore takes the value it expects as
// an argument and re-checks it under the same lock a wake must hold, so
// the check and the enqueue are one atomic step -- exactly the
// release-lock-then-block handoff waitq_sleep already implements for
// mutexes.

#include "futex.h"
#include "lock.h"
#include "waitq.h"
#include "serial.h"
#include "errno.h"
#include "signal.h"
#include "timer.h"
#include "mm/paging.h"
#include "sched/proc.h"

// A power of two so the index is a mask, and comfortably larger than
// the number of futexes any NeoOS program has yet had. Collisions are a
// correctness non-event -- every waiter carries its key and a wake only
// touches matching ones -- so this is purely about how long the walk is.
#define FUTEX_BUCKETS 64

// One waiter, living on the SLEEPING THREAD'S OWN KERNEL STACK. That is
// deliberate: the record's lifetime is exactly the sleep, so there is
// nothing to allocate, nothing to free, and no way to leak one if the
// sleep is interrupted. It is safe because a thread cannot return from
// waitq_sleep -- and so cannot unwind the frame holding this -- until
// the waker has finished with it.
struct futex_waiter {
    struct futex_waiter *next;
    uint64_t             key;
    struct waitq         q;
    int                  woken;   // 1 = a real FUTEX_WAKE, not a timeout or signal
};

struct futex_bucket {
    struct spinlock      lock;
    struct futex_waiter *waiters;
};

static struct futex_bucket buckets[FUTEX_BUCKETS];

void futex_init(void) {
    for (int i = 0; i < FUTEX_BUCKETS; i++) {
        spin_init(&buckets[i].lock, LOCK_RANK_FUTEX, "futex");
        buckets[i].waiters = 0;
    }
}

// Multiplicative hash over the physical address. The low 2 bits are
// always zero (futex words are 4-byte aligned) and would waste a
// quarter of the table if used directly.
static struct futex_bucket *bucket_for(uint64_t key) {
    uint64_t h = key >> 2;
    h *= 0x9E3779B97F4A7C15ULL;   // 2^64 / phi
    return &buckets[(h >> 58) & (FUTEX_BUCKETS - 1)];
}

// The futex's identity is its PHYSICAL address.
//
// Keying by (address space, virtual address) would be simpler and would
// work for every futex NeoOS can create today, since a private mapping
// has exactly one user. It would then be wrong the moment two processes
// share a page -- which is precisely what a process-shared semaphore,
// and later MPI's shared-memory fast path, are for. A physical key is
// right for both cases at once and costs one page-table walk.
//
// The address is stable for the duration because the caller has already
// been through user_range_writable, which breaks copy-on-write sharing.
// A COW page IS a different futex in each process afterwards, which is
// what MAP_PRIVATE means and what Linux does.
static uint64_t futex_key(uint32_t *uaddr) {
    return paging_translate_current((uint64_t)(uintptr_t)uaddr);
}

static int64_t futex_wait(uint32_t *uaddr, uint32_t val,
                          const struct k_timespec *timeout) {
    uint64_t key = futex_key(uaddr);
    if (!key) { return -EFAULT; }

    struct futex_bucket *b = bucket_for(key);
    struct futex_waiter w;
    w.key   = key;
    w.woken = 0;
    waitq_init(&w.q);

    uint64_t deadline = 0;
    if (timeout) {
        // Relative, as Linux's FUTEX_WAIT is (FUTEX_WAIT_BITSET is the
        // absolute one, and is not implemented). Rounded UP to a whole
        // tick: a timeout shorter than the 10ms tick must still sleep,
        // not return immediately.
        uint64_t ticks = (uint64_t)timeout->tv_sec * TIMER_HZ
                       + ((uint64_t)timeout->tv_nsec + (1000000000UL / TIMER_HZ) - 1)
                         / (1000000000UL / TIMER_HZ);
        deadline = timer_ticks() + (ticks ? ticks : 1);
    }

    uint64_t f = spin_lock_irqsave(&b->lock);

    // THE atomic step. A waker must hold this same lock, so a value
    // change published before a FUTEX_WAKE is necessarily visible here,
    // and a change published after it cannot lose the wake -- we are
    // already on the list by the time the lock is dropped.
    if (__atomic_load_n(uaddr, __ATOMIC_ACQUIRE) != val) {
        spin_unlock_irqrestore(&b->lock, f);
        return -EAGAIN;
    }

    w.next = b->waiters;
    b->waiters = &w;

    // waitq_sleep enqueues under the wait queue's own lock and only
    // THEN releases the bucket lock, so there is no window between
    // becoming visible to a waker and becoming asleep.
    int rc = timeout ? waitq_sleep_timeout(&w.q, &b->lock, deadline)
                     : waitq_sleep(&w.q, &b->lock);
    // waitq_sleep re-acquires `release` on the way out.

    // Unlink if still listed. A real wake has already removed us; a
    // timeout or a signal has not, and leaving the record on the list
    // would leave a pointer into a stack frame that is about to go away.
    if (!w.woken) {
        struct futex_waiter **pp = &b->waiters;
        while (*pp && *pp != &w) { pp = &(*pp)->next; }
        if (*pp) { *pp = w.next; }
    }
    spin_unlock_irqrestore(&b->lock, f);

    if (w.woken)             { return 0; }
    if (rc == -ETIMEDOUT)    { return -ETIMEDOUT; }
    if (rc == -EINTR)        { return -EINTR; }
    // Woken by something that was not a matching FUTEX_WAKE and was not
    // an error either. Linux permits a spurious return here, and every
    // futex user re-checks its own condition in a loop.
    return 0;
}

static int64_t futex_wake(uint32_t *uaddr, uint32_t val) {
    uint64_t key = futex_key(uaddr);
    if (!key) { return -EFAULT; }

    struct futex_bucket *b = bucket_for(key);
    int64_t woken = 0;

    uint64_t f = spin_lock_irqsave(&b->lock);
    struct futex_waiter **pp = &b->waiters;
    while (*pp && (uint32_t)woken < val) {
        struct futex_waiter *w = *pp;
        if (w->key != key) { pp = &w->next; continue; }

        // Unlink BEFORE waking, so the sleeper's own cleanup finds
        // itself gone and does not walk a list it no longer belongs to.
        *pp = w->next;
        w->woken = 1;
        // Woken under the bucket lock on purpose. waitq_wake_one can
        // spin waiting for the sleeper to finish leaving its CPU, but
        // the sleeper is not waiting on this lock -- waitq_sleep
        // released it before scheduling -- so the wait is bounded and
        // cannot deadlock.
        waitq_wake_one(&w->q);
        woken++;
    }
    spin_unlock_irqrestore(&b->lock, f);
    return woken;
}

int64_t futex_op(uint32_t *uaddr, int op, uint32_t val,
                 const struct k_timespec *timeout) {
    // Alignment is not pedantry: the word is read with an atomic load,
    // and a 4-byte load spanning two pages is neither atomic nor
    // necessarily mapped. Linux returns EINVAL for the same reason.
    if (!uaddr || ((uintptr_t)uaddr & 3)) { return -EINVAL; }
    if (!user_range_writable((uint64_t)(uintptr_t)uaddr, sizeof(uint32_t))) {
        return -EFAULT;
    }

    int cmd = op & FUTEX_CMD_MASK;
    if (cmd == FUTEX_WAIT) { return futex_wait(uaddr, val, timeout); }
    if (cmd == FUTEX_WAKE) { return futex_wake(uaddr, val); }
    return -ENOSYS;
}

// ---------------------------------------------------------------- selftest

static uint32_t selftest_word;
static volatile int selftest_stage;

// The selftest's futex word is a kernel address, so it goes through
// futex_wait/futex_wake DIRECTLY rather than through futex_op --
// futex_op's user_range_writable gate rejects the upper half, correctly
// and by design. That gate is checked separately, below. What these
// threads exercise is the handoff, which is the part that can be got
// wrong silently.
static void futex_sleeper(void) {
    selftest_word = 1;
    selftest_stage = 1;

    // Deliberately the WRONG expected value: this must return -EAGAIN
    // immediately rather than blocking, which is the check that saves a
    // caller from sleeping through a wake that already happened.
    if (futex_wait(&selftest_word, 999, 0) != -EAGAIN) {
        serial_write_string("[futex] selftest FAILED: mismatched value did not return EAGAIN\n");
        selftest_stage = 99;
        thread_exit_self(1);
    }

    selftest_stage = 2;
    if (futex_wait(&selftest_word, 1, 0) != 0) {
        serial_write_string("[futex] selftest FAILED: wait returned nonzero\n");
        selftest_stage = 99;
        thread_exit_self(1);
    }
    selftest_stage = 3;
    thread_exit_self(0);
}

static void futex_selftest_thread(void) {
    selftest_stage = 0;
    selftest_word  = 0;

    // A kernel pointer must be refused. This is the check that stops a
    // user program from using FUTEX_WAIT to probe, or park on, kernel
    // memory.
    if (futex_op(&selftest_word, FUTEX_WAIT, 0, 0) != -EFAULT) {
        serial_write_string("[futex] selftest FAILED: a kernel address was not refused\n");
        thread_exit_self(1);
    }
    // And a misaligned one, since the value is read with an atomic load.
    if (futex_op((uint32_t *)((uintptr_t)&selftest_word + 1),
                 FUTEX_WAIT, 0, 0) != -EINVAL) {
        serial_write_string("[futex] selftest FAILED: a misaligned address was not refused\n");
        thread_exit_self(1);
    }

    if (!thread_alloc_kernel(futex_sleeper)) {
        serial_write_string("[futex] selftest FAILED: thread_alloc_kernel\n");
        thread_exit_self(1);
    }

    while (selftest_stage < 2) { schedule(); }
    if (selftest_stage == 99) { thread_exit_self(1); }

    // Give the sleeper time to actually reach the queue. Waking an
    // empty queue would "pass" without proving anything, so this waits
    // for a waiter to be there and fails if none ever arrives.
    int64_t woke = 0;
    for (int i = 0; i < 200000 && woke == 0; i++) {
        woke = futex_wake(&selftest_word, 1);
        if (woke == 0) { schedule(); }
    }
    if (woke != 1) {
        serial_write_string("[futex] selftest FAILED: wake did not find the sleeper\n");
        thread_exit_self(1);
    }

    while (selftest_stage != 3) { schedule(); }

    // Waking an empty futex must be a no-op returning 0, not an error:
    // every unlock does it in the uncontended case.
    if (futex_wake(&selftest_word, 1) != 0) {
        serial_write_string("[futex] selftest FAILED: wake on an empty futex did not return 0\n");
        thread_exit_self(1);
    }

    serial_write_string("[futex] selftest passed\n");
    thread_exit_self(0);
}

void futex_selftest(void) {
    thread_alloc_kernel(futex_selftest_thread);
}
