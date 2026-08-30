#ifndef NEOOS_PTHREAD_H
#define NEOOS_PTHREAD_H

#include <time.h>

// A SUBSET of POSIX threads: creation, joining, mutexes and condition
// variables, all over the futex. What is here has POSIX's names,
// signatures and semantics; what is not here is simply absent, so a
// program that needs it fails to compile rather than linking against a
// stub that lies at run time. See docs/stdlib.md for the full list of
// what is missing (attributes, cancellation, TLS keys, rwlocks,
// barriers, spinlocks) and what each omission costs.
//
// This header exists because "POSIX mutex" means pthread_mutex_t to
// every program that wants one; providing the mutexes without
// pthread_create would be a header that compiles and then fails to
// link. When musl is integrated it supplies all of this and this file
// goes away -- the kernel side (the futex) is what survives, which is
// the point of building on a Linux-shaped primitive.

typedef int pthread_t;

// Recursive and error-checking mutexes are not implemented, so there is
// no attribute type to select them. A normal mutex it is.
typedef struct {
    // 0 = unlocked, 1 = locked with no waiters, 2 = locked and at
    // least one thread may be sleeping. The third state is what lets an
    // uncontended unlock skip the syscall: only a 2 obliges the
    // unlocker to wake anybody. (Drepper, "Futexes Are Tricky", mutex3.)
    volatile int state;
} pthread_mutex_t;

typedef struct {
    // A sequence number, not a waiter count. Every signal bumps it, and
    // a waiter sleeps only while it still reads what it read before
    // dropping the mutex -- so a signal that lands in that window
    // cannot be missed. See pthread_cond_wait.
    volatile int seq;
} pthread_cond_t;

#define PTHREAD_MUTEX_INITIALIZER { 0 }
#define PTHREAD_COND_INITIALIZER  { 0 }

// `attr` must be null: no attribute type exists. Passing anything else
// returns -EINVAL rather than silently ignoring it.
int pthread_create(pthread_t *thread, const void *attr,
                   void *(*start_routine)(void *), void *arg);
int pthread_join(pthread_t thread, void **retval);
void pthread_exit(void *retval) __attribute__((noreturn));
pthread_t pthread_self(void);
int pthread_equal(pthread_t a, pthread_t b);

int pthread_mutex_init(pthread_mutex_t *m, const void *attr);
int pthread_mutex_destroy(pthread_mutex_t *m);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);

int pthread_cond_init(pthread_cond_t *c, const void *attr);
int pthread_cond_destroy(pthread_cond_t *c);
// Wakeups may be spurious, as POSIX permits: always call this in a loop
// that re-tests the predicate.
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m);
// RELATIVE timeout, unlike POSIX's pthread_cond_timedwait, which takes
// an absolute one -- NeoOS has no clock syscall to build an absolute
// deadline from. Named differently so the divergence cannot be reached
// by accident. Recorded in docs/stdlib.md.
int pthread_cond_timedwait_relative(pthread_cond_t *c, pthread_mutex_t *m,
                                    const struct timespec *rel);
int pthread_cond_signal(pthread_cond_t *c);
int pthread_cond_broadcast(pthread_cond_t *c);

#endif
