#include <pthread.h>
#include <futex.h>
#include <thread.h>
#include <errno.h>

// ---------------------------------------------------------------- mutex
//
// Drepper's three-state futex mutex ("Futexes Are Tricky", mutex3).
// The three states are what keep the uncontended path free of syscalls:
//
//   0  unlocked
//   1  locked, and no thread has ever had to sleep on it
//   2  locked, and a thread may be sleeping
//
// Only state 2 obliges the unlocker to make a futex_wake call. A lock
// and unlock pair with no contention is therefore two atomic
// instructions and no kernel entry at all -- which is the entire reason
// to build on a futex rather than on a blocking syscall.
//
// The subtle line is the `if (c != 2) c = xchg(m, 2)` below. A thread
// that is about to sleep must publish state 2 FIRST, because the
// unlocker reads the state to decide whether to wake anyone. Setting it
// with an exchange (rather than a store) also returns the previous
// value, so a lock that was released in that instant is picked up
// without sleeping.

int pthread_mutex_init(pthread_mutex_t *m, const void *attr) {
    if (!m) { return -EINVAL; }
    if (attr) { return -EINVAL; }   // no attribute type exists; see the header
    __atomic_store_n(&m->state, 0, __ATOMIC_RELEASE);
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *m) {
    if (!m) { return -EINVAL; }
    return 0;   // owns no kernel object
}

int pthread_mutex_trylock(pthread_mutex_t *m) {
    if (!m) { return -EINVAL; }
    int expected = 0;
    if (__atomic_compare_exchange_n(&m->state, &expected, 1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        return 0;
    }
    return -EBUSY;
}

int pthread_mutex_lock(pthread_mutex_t *m) {
    if (!m) { return -EINVAL; }

    int expected = 0;
    if (__atomic_compare_exchange_n(&m->state, &expected, 1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        return 0;   // uncontended: no syscall
    }

    int c = expected;   // whatever the state actually was
    if (c != 2) { c = __atomic_exchange_n(&m->state, 2, __ATOMIC_ACQUIRE); }
    while (c != 0) {
        // Sleeps only while the state is still 2. An unlock that lands
        // between the exchange above and this call sets the state to 0,
        // the kernel's comparison fails, and this returns -EAGAIN
        // immediately -- so the wake cannot be lost.
        futex_wait((int *)&m->state, 2);
        c = __atomic_exchange_n(&m->state, 2, __ATOMIC_ACQUIRE);
    }
    return 0;
}

int pthread_mutex_unlock(pthread_mutex_t *m) {
    if (!m) { return -EINVAL; }
    // fetch_sub, not a store: the return value distinguishes "was 1, so
    // nobody is waiting" from "was 2, so somebody might be".
    if (__atomic_fetch_sub(&m->state, 1, __ATOMIC_RELEASE) != 1) {
        __atomic_store_n(&m->state, 0, __ATOMIC_RELEASE);
        futex_wake((int *)&m->state, 1);
    }
    return 0;
}

// ------------------------------------------------------- condition variable
//
// The sequence-number condvar. `seq` is bumped by every signal, and a
// waiter sleeps only while seq still reads what it read BEFORE dropping
// the mutex. A signal landing in the window between the unlock and the
// sleep therefore changes seq, the kernel's comparison fails, and the
// wait returns immediately instead of sleeping through the wakeup.
//
// It is deliberately the simple version. Signal wakes one waiter and
// broadcast wakes all of them, which means a broadcast is a thundering
// herd: every woken thread immediately contends for the mutex and all
// but one goes back to sleep. musl avoids that with FUTEX_REQUEUE,
// moving waiters from the condvar's futex to the mutex's without ever
// running them. NeoOS's kernel implements only WAIT and WAKE, so that
// optimisation is not available here -- and it is an optimisation, not
// a correctness matter. Recorded in docs/stdlib.md.

int pthread_cond_init(pthread_cond_t *c, const void *attr) {
    if (!c) { return -EINVAL; }
    if (attr) { return -EINVAL; }
    __atomic_store_n(&c->seq, 0, __ATOMIC_RELEASE);
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *c) {
    if (!c) { return -EINVAL; }
    return 0;
}

static int cond_wait_common(pthread_cond_t *c, pthread_mutex_t *m,
                            const struct timespec *rel) {
    if (!c || !m) { return -EINVAL; }

    // Read BEFORE releasing the mutex. This is the whole mechanism: any
    // signal from here on necessarily changes seq, and a changed seq is
    // what stops the sleep below from happening.
    int seq = __atomic_load_n(&c->seq, __ATOMIC_ACQUIRE);

    pthread_mutex_unlock(m);
    int rc = rel ? futex_wait_timeout((int *)&c->seq, seq, rel)
                 : futex_wait((int *)&c->seq, seq);
    pthread_mutex_lock(m);

    // -EAGAIN means seq had already moved: a signal we must not treat
    // as a timeout. Everything else other than a real timeout is a
    // permitted spurious wakeup, which the caller's loop absorbs.
    if (rc == -ETIMEDOUT) { return -ETIMEDOUT; }
    return 0;
}

int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) {
    return cond_wait_common(c, m, 0);
}

int pthread_cond_timedwait_relative(pthread_cond_t *c, pthread_mutex_t *m,
                                    const struct timespec *rel) {
    if (!rel) { return -EINVAL; }
    return cond_wait_common(c, m, rel);
}

int pthread_cond_signal(pthread_cond_t *c) {
    if (!c) { return -EINVAL; }
    __atomic_fetch_add(&c->seq, 1, __ATOMIC_RELEASE);
    futex_wake((int *)&c->seq, 1);
    return 0;
}

int pthread_cond_broadcast(pthread_cond_t *c) {
    if (!c) { return -EINVAL; }
    __atomic_fetch_add(&c->seq, 1, __ATOMIC_RELEASE);
    // INT_MAX rather than a counted wake: nothing tracks how many
    // waiters there are, and the kernel stops at however many it finds.
    futex_wake((int *)&c->seq, 0x7FFFFFFF);
    return 0;
}

// ---------------------------------------------------------------- threads
//
// pthread_create's start routine returns void *; NeoOS's thread_exit
// takes an int. The bridge is a small table of return values, indexed
// by nothing cleverer than a linear scan -- there are at most
// MAX_THREADS_PER_PROC threads, and a join is not a hot path.
//
// The alternative, storing the return value in thread-local storage,
// needs TLS, which needs the FS base to be part of the context switch.
// That is a later milestone; this table is what makes pthread usable
// before it lands, and is meant to be deleted when musl arrives.

#define PTHREAD_MAX 16

struct pthread_slot {
    volatile int in_use;
    volatile int tid;        // 0 until the thread has started
    volatile int finished;
    void *(*start)(void *);
    void  *arg;
    void  *retval;
};

static struct pthread_slot pt_slots[PTHREAD_MAX];
static pthread_mutex_t     pt_lock = PTHREAD_MUTEX_INITIALIZER;

static struct pthread_slot *slot_for_tid(int tid) {
    for (int i = 0; i < PTHREAD_MAX; i++) {
        if (pt_slots[i].in_use && pt_slots[i].tid == tid) { return &pt_slots[i]; }
    }
    return 0;
}

static void pthread_trampoline(void *raw) {
    struct pthread_slot *s = (struct pthread_slot *)raw;
    // Recorded by the thread ITSELF rather than by the creator: the
    // creator only learns the tid after thread_create returns, which
    // may be after this thread has already run and exited.
    __atomic_store_n(&s->tid, thread_self(), __ATOMIC_RELEASE);
    void *r = s->start(s->arg);
    s->retval = r;
    __atomic_store_n(&s->finished, 1, __ATOMIC_RELEASE);
    thread_exit(0);
}

int pthread_create(pthread_t *thread, const void *attr,
                   void *(*start_routine)(void *), void *arg) {
    if (!start_routine) { return -EINVAL; }
    if (attr) { return -EINVAL; }   // no attribute type exists; see the header

    pthread_mutex_lock(&pt_lock);
    struct pthread_slot *s = 0;
    for (int i = 0; i < PTHREAD_MAX; i++) {
        if (!pt_slots[i].in_use) { s = &pt_slots[i]; break; }
    }
    if (!s) { pthread_mutex_unlock(&pt_lock); return -EAGAIN; }
    s->in_use   = 1;
    s->tid      = 0;
    s->finished = 0;
    s->start    = start_routine;
    s->arg      = arg;
    s->retval   = 0;
    pthread_mutex_unlock(&pt_lock);

    thread_t t;
    int rc = thread_create(&t, pthread_trampoline, s);
    if (rc != 0) {
        pthread_mutex_lock(&pt_lock);
        s->in_use = 0;
        pthread_mutex_unlock(&pt_lock);
        return rc;
    }
    if (thread) { *thread = t; }
    return 0;
}

int pthread_join(pthread_t thread, void **retval) {
    // thread_join does the actual waiting and reaping; the slot only
    // carries the return value across.
    int code = 0;
    int rc = thread_join(thread, &code);
    if (rc != 0) { return rc; }

    pthread_mutex_lock(&pt_lock);
    struct pthread_slot *s = slot_for_tid(thread);
    if (s) {
        if (retval) { *retval = s->retval; }
        s->in_use = 0;
    } else if (retval) {
        // Joined a thread this library did not create -- legal, since
        // <thread.h> is still available alongside. There is no void *
        // to report.
        *retval = 0;
    }
    pthread_mutex_unlock(&pt_lock);
    return 0;
}

void pthread_exit(void *retval) {
    int self = thread_self();
    pthread_mutex_lock(&pt_lock);
    struct pthread_slot *s = slot_for_tid(self);
    if (s) {
        s->retval = retval;
        __atomic_store_n(&s->finished, 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&pt_lock);
    thread_exit(0);
}

pthread_t pthread_self(void) { return thread_self(); }

int pthread_equal(pthread_t a, pthread_t b) { return a == b; }
