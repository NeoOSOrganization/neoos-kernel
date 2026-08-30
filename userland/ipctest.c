// Exercises the futex and everything built on it: POSIX semaphores,
// pthread mutexes, condition variables, and the pthread thread calls.
//
// The checks are chosen to fail if the SYNCHRONISATION is wrong, not
// merely if the API is missing. A mutex that does not actually exclude
// shows up as a lost increment; a condvar with a lost wakeup shows up
// as a hang, which the test harness catches as a missing marker.

#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <futex.h>
#include <semaphore.h>
#include <pthread.h>

#define WORKERS     4
#define INCREMENTS  2000

// ------------------------------------------------------------ raw futex

static volatile int futex_word;
static volatile int futex_woke;

static void *futex_sleeper(void *arg) {
    (void)arg;
    // Sleeps only while the word still reads 0. If the main thread's
    // store and wake land before this call, the kernel's comparison
    // fails and this returns -EAGAIN rather than sleeping forever --
    // which is the property being tested.
    while (__atomic_load_n(&futex_word, __ATOMIC_ACQUIRE) == 0) {
        futex_wait((int *)&futex_word, 0);
    }
    __atomic_fetch_add((int *)&futex_woke, 1, __ATOMIC_ACQ_REL);
    return 0;
}

static int check_futex(void) {
    // A mismatched expectation must not block.
    __atomic_store_n(&futex_word, 7, __ATOMIC_RELEASE);
    if (futex_wait((int *)&futex_word, 0) != -EAGAIN) {
        printf("[ipctest] FAILED: futex_wait on a changed value did not return EAGAIN\n");
        return 0;
    }
    // Waking an idle futex is a no-op, not an error.
    if (futex_wake((int *)&futex_word, 1) != 0) {
        printf("[ipctest] FAILED: futex_wake with no waiters did not return 0\n");
        return 0;
    }

    __atomic_store_n(&futex_word, 0, __ATOMIC_RELEASE);
    futex_woke = 0;

    pthread_t t[WORKERS];
    for (int i = 0; i < WORKERS; i++) {
        if (pthread_create(&t[i], 0, futex_sleeper, 0) != 0) {
            printf("[ipctest] FAILED: pthread_create for futex sleeper %d\n", i);
            return 0;
        }
    }

    // Let them reach the wait. Not required for correctness -- that is
    // the point of the value check -- but it makes the test exercise
    // the sleeping path rather than the fast path.
    for (volatile int i = 0; i < 400000; i++) { }

    __atomic_store_n(&futex_word, 1, __ATOMIC_RELEASE);
    futex_wake((int *)&futex_word, WORKERS);

    for (int i = 0; i < WORKERS; i++) {
        if (pthread_join(t[i], 0) != 0) {
            printf("[ipctest] FAILED: pthread_join for futex sleeper %d\n", i);
            return 0;
        }
    }
    if (futex_woke != WORKERS) {
        printf("[ipctest] FAILED: %d of %d futex sleepers woke\n", futex_woke, WORKERS);
        return 0;
    }
    printf("[ipctest] futex wait/wake passed\n");
    return 1;
}

// ---------------------------------------------------------------- mutex

static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static long            guarded_counter;   // deliberately NOT atomic

static void *mutex_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < INCREMENTS; i++) {
        pthread_mutex_lock(&mtx);
        // A plain read-modify-write. If the mutex does not actually
        // exclude -- and with work stealing these threads really do run
        // on different CPUs -- increments are lost and the total below
        // comes out short.
        guarded_counter = guarded_counter + 1;
        pthread_mutex_unlock(&mtx);
    }
    return 0;
}

static int check_mutex(void) {
    guarded_counter = 0;

    if (pthread_mutex_trylock(&mtx) != 0) {
        printf("[ipctest] FAILED: trylock on a free mutex\n");
        return 0;
    }
    if (pthread_mutex_trylock(&mtx) != -EBUSY) {
        printf("[ipctest] FAILED: trylock on a held mutex did not return EBUSY\n");
        return 0;
    }
    pthread_mutex_unlock(&mtx);

    pthread_t t[WORKERS];
    for (int i = 0; i < WORKERS; i++) {
        if (pthread_create(&t[i], 0, mutex_worker, 0) != 0) {
            printf("[ipctest] FAILED: pthread_create for mutex worker %d\n", i);
            return 0;
        }
    }
    for (int i = 0; i < WORKERS; i++) { pthread_join(t[i], 0); }

    if (guarded_counter != (long)WORKERS * INCREMENTS) {
        printf("[ipctest] FAILED: mutex lost increments, %d of %d\n",
               (int)guarded_counter, WORKERS * INCREMENTS);
        return 0;
    }
    printf("[ipctest] pthread mutex passed, count=%d\n", (int)guarded_counter);
    return 1;
}

// ------------------------------------------------------------ semaphore

static sem_t sem;
static volatile int sem_in_section;      // how many hold the semaphore now
static volatile int sem_max_observed;    // the high-water mark

static void *sem_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 60; i++) {
        sem_wait(&sem);
        int now = __atomic_add_fetch((int *)&sem_in_section, 1, __ATOMIC_ACQ_REL);
        // Records the peak. A semaphore initialised to 2 must never let
        // a third thread in, so this is the assertion that the count is
        // being honoured rather than the semaphore just being a lock.
        int seen = __atomic_load_n((int *)&sem_max_observed, __ATOMIC_ACQUIRE);
        while (now > seen) {
            if (__atomic_compare_exchange_n((int *)&sem_max_observed, &seen, now,
                                            1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                break;
            }
        }
        // Long enough that two holders genuinely overlap. With a
        // one-instruction critical section the peak was always 1, so
        // the "never more than 2" assertion never had anything to
        // reject -- it would have passed just as happily against a
        // plain mutex.
        for (volatile int k = 0; k < 30000; k++) { }
        __atomic_fetch_sub((int *)&sem_in_section, 1, __ATOMIC_ACQ_REL);
        sem_post(&sem);
    }
    return 0;
}

static int check_semaphore(void) {
    if (sem_init(&sem, 0, 2) != 0) {
        printf("[ipctest] FAILED: sem_init\n");
        return 0;
    }
    int v = -1;
    if (sem_getvalue(&sem, &v) != 0 || v != 2) {
        printf("[ipctest] FAILED: sem_getvalue reported %d, want 2\n", v);
        return 0;
    }
    // Drain it, then prove trywait refuses rather than blocking.
    if (sem_trywait(&sem) != 0 || sem_trywait(&sem) != 0) {
        printf("[ipctest] FAILED: sem_trywait on an available semaphore\n");
        return 0;
    }
    if (sem_trywait(&sem) != -EAGAIN) {
        printf("[ipctest] FAILED: sem_trywait on an empty semaphore did not return EAGAIN\n");
        return 0;
    }
    sem_post(&sem);
    sem_post(&sem);

    sem_in_section   = 0;
    sem_max_observed = 0;

    pthread_t t[WORKERS];
    for (int i = 0; i < WORKERS; i++) {
        if (pthread_create(&t[i], 0, sem_worker, 0) != 0) {
            printf("[ipctest] FAILED: pthread_create for sem worker %d\n", i);
            return 0;
        }
    }
    for (int i = 0; i < WORKERS; i++) { pthread_join(t[i], 0); }

    if (sem_max_observed > 2) {
        printf("[ipctest] FAILED: %d threads held a semaphore of 2 at once\n",
               sem_max_observed);
        return 0;
    }
    if (sem_max_observed < 1) {
        printf("[ipctest] FAILED: no thread ever entered the semaphore\n");
        return 0;
    }
    sem_getvalue(&sem, &v);
    if (v != 2) {
        printf("[ipctest] FAILED: semaphore ended at %d, want 2\n", v);
        return 0;
    }
    sem_destroy(&sem);
    printf("[ipctest] semaphore passed, peak concurrency=%d\n", sem_max_observed);
    return 1;
}

// ------------------------------------------------------- condition variable

static pthread_mutex_t cv_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv     = PTHREAD_COND_INITIALIZER;
static int             cv_ready;      // the predicate, guarded by cv_mtx
static volatile int    cv_awake;

static void *cond_worker(void *arg) {
    (void)arg;
    pthread_mutex_lock(&cv_mtx);
    // A loop, not an if: wakeups are permitted to be spurious, and the
    // predicate is the truth.
    while (!cv_ready) {
        pthread_cond_wait(&cv, &cv_mtx);
    }
    pthread_mutex_unlock(&cv_mtx);
    __atomic_fetch_add((int *)&cv_awake, 1, __ATOMIC_ACQ_REL);
    return 0;
}

static int check_condvar(void) {
    cv_ready = 0;
    cv_awake = 0;

    pthread_t t[WORKERS];
    for (int i = 0; i < WORKERS; i++) {
        if (pthread_create(&t[i], 0, cond_worker, 0) != 0) {
            printf("[ipctest] FAILED: pthread_create for cond worker %d\n", i);
            return 0;
        }
    }

    // Deliberately NOT waiting for the workers to reach the wait. If
    // the broadcast lands before they get there, the sequence number
    // has already moved and pthread_cond_wait returns immediately --
    // and the predicate is already true, so the loop exits. A hang here
    // would mean the condvar loses wakeups; the harness reports it as a
    // missing "[ipctest] ALL PASSED" marker.
    pthread_mutex_lock(&cv_mtx);
    cv_ready = 1;
    pthread_mutex_unlock(&cv_mtx);
    pthread_cond_broadcast(&cv);

    for (int i = 0; i < WORKERS; i++) { pthread_join(t[i], 0); }

    if (cv_awake != WORKERS) {
        printf("[ipctest] FAILED: %d of %d threads woke from the condvar\n",
               cv_awake, WORKERS);
        return 0;
    }
    printf("[ipctest] condition variable passed\n");
    return 1;
}

// ------------------------------------------------------ pthread plumbing

static void *retval_worker(void *arg) {
    return (void *)((long)arg * 3);
}

static int check_pthread_retval(void) {
    pthread_t t;
    if (pthread_create(&t, 0, retval_worker, (void *)14L) != 0) {
        printf("[ipctest] FAILED: pthread_create for retval worker\n");
        return 0;
    }
    if (pthread_equal(t, pthread_self())) {
        printf("[ipctest] FAILED: a new thread compared equal to self\n");
        return 0;
    }
    void *r = 0;
    if (pthread_join(t, &r) != 0) {
        printf("[ipctest] FAILED: pthread_join for retval worker\n");
        return 0;
    }
    if ((long)r != 42) {
        printf("[ipctest] FAILED: join got %d, want 42\n", (int)(long)r);
        return 0;
    }
    // Attributes are not implemented, and must be refused rather than
    // silently ignored.
    if (pthread_create(&t, (const void *)1, retval_worker, 0) != -EINVAL) {
        printf("[ipctest] FAILED: a non-null attr was not rejected\n");
        return 0;
    }
    printf("[ipctest] pthread create/join/retval passed\n");
    return 1;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    int ok = 1;
    ok &= check_futex();
    ok &= check_mutex();
    ok &= check_semaphore();
    ok &= check_condvar();
    ok &= check_pthread_retval();

    printf("[ipctest] %s\n", ok ? "ALL PASSED" : "SOME CHECKS FAILED");
    return ok ? 0 : 1;
}
