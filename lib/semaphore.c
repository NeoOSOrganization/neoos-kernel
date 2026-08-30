#include <semaphore.h>
#include <futex.h>
#include <errno.h>

int sem_init(sem_t *sem, int pshared, unsigned int value) {
    if (!sem) { return -EINVAL; }
    sem->pshared = pshared;
    // Release, so the count is visible to any thread that later reads
    // it -- including one that finds the semaphore through a pointer
    // published right after this call.
    __atomic_store_n(&sem->value, (int)value, __ATOMIC_RELEASE);
    return 0;
}

int sem_destroy(sem_t *sem) {
    if (!sem) { return -EINVAL; }
    // Nothing to release: the semaphore owns no kernel object. A futex
    // exists only while someone is blocked on it, and destroying a
    // semaphore with waiters is undefined behaviour in POSIX too.
    return 0;
}

int sem_trywait(sem_t *sem) {
    if (!sem) { return -EINVAL; }
    int v = __atomic_load_n(&sem->value, __ATOMIC_ACQUIRE);
    while (v > 0) {
        // Weak is correct here: a spurious failure just re-reads v and
        // goes round again, and weak is cheaper on architectures where
        // the difference exists.
        if (__atomic_compare_exchange_n(&sem->value, &v, v - 1, 1,
                                        __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
            return 0;
        }
    }
    return -EAGAIN;
}

// The wait loop, shared by sem_wait and sem_timedwait_relative.
//
// The order is what makes it correct: the count is re-read, and the
// futex wait is told to sleep only while the count is still zero. A
// sem_post landing between the read and the sleep therefore either
// makes the count non-zero (so the loop takes the fast path next time
// round) or fails the kernel's comparison and returns -EAGAIN
// immediately. There is no window in which a post can be lost.
static int sem_wait_common(sem_t *sem, const struct timespec *rel) {
    if (!sem) { return -EINVAL; }
    for (;;) {
        int v = __atomic_load_n(&sem->value, __ATOMIC_ACQUIRE);
        if (v > 0) {
            if (__atomic_compare_exchange_n(&sem->value, &v, v - 1, 1,
                                            __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
                return 0;
            }
            continue;   // lost the race to another waiter; look again
        }

        int rc = rel ? futex_wait_timeout((int *)&sem->value, 0, rel)
                     : futex_wait((int *)&sem->value, 0);
        if (rc == -ETIMEDOUT) { return -ETIMEDOUT; }
        // -EAGAIN means the count changed under us, and -EINTR means a
        // signal arrived. Both simply mean "look again": this call does
        // not report EINTR, because a semaphore whose acquisition can
        // fail spuriously pushes the retry loop into every caller.
    }
}

int sem_wait(sem_t *sem) { return sem_wait_common(sem, 0); }

int sem_timedwait_relative(sem_t *sem, const struct timespec *rel) {
    if (!rel) { return -EINVAL; }
    return sem_wait_common(sem, rel);
}

int sem_post(sem_t *sem) {
    if (!sem) { return -EINVAL; }
    __atomic_fetch_add(&sem->value, 1, __ATOMIC_RELEASE);
    // Woken unconditionally rather than only when waiters are known to
    // exist. Tracking a waiter count would save a syscall on the
    // uncontended post, but it is a second piece of shared state to
    // keep consistent with the first, and getting it wrong loses
    // wakeups. futex_wake on an empty futex is a cheap no-op; this is
    // the trade made deliberately, and is where to look first if
    // sem_post ever shows up in a profile.
    futex_wake((int *)&sem->value, 1);
    return 0;
}

int sem_getvalue(sem_t *sem, int *value) {
    if (!sem || !value) { return -EINVAL; }
    *value = __atomic_load_n(&sem->value, __ATOMIC_ACQUIRE);
    return 0;
}
