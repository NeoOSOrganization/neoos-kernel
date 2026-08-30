#ifndef NEOOS_SEMAPHORE_H
#define NEOOS_SEMAPHORE_H

#include <time.h>

// POSIX unnamed semaphores, over the futex.
//
// The count IS the futex word, so an uncontended sem_wait or sem_post
// is one atomic instruction and no syscall at all. sem_t is a plain
// struct rather than an opaque byte array: NeoOS has no ABI to freeze
// here yet, and a readable definition is worth more than a placeholder
// that will be replaced by musl's anyway.

typedef struct {
    // Signed on purpose. It never goes negative in this implementation,
    // but sem_getvalue is specified to be able to report a negative
    // value (the number of waiters) on systems that track them, and
    // keeping the type signed leaves that door open.
    volatile int value;
    int          pshared;
} sem_t;

// `pshared` is accepted and honoured: the kernel keys futexes by
// physical address, so a semaphore in shared memory works between
// processes with no extra bookkeeping. It is stored only so
// sem_getvalue and a future sem_destroy can be strict about it.
int sem_init(sem_t *sem, int pshared, unsigned int value);
int sem_destroy(sem_t *sem);

// Blocks until the count is positive, then decrements it. Returns 0, or
// -EINVAL. Unlike POSIX's, this returns the negative error directly and
// does NOT return -EINTR: a signal that arrives mid-wait resumes the
// wait rather than failing it. See docs/stdlib.md for why.
int sem_wait(sem_t *sem);

// Decrements if the count is positive; never blocks. Returns 0, or
// -EAGAIN if the count was zero.
int sem_trywait(sem_t *sem);

// As sem_wait, with a RELATIVE timeout (POSIX's sem_timedwait takes an
// ABSOLUTE one -- a deliberate divergence, recorded in docs/stdlib.md,
// because NeoOS has no clock syscall to build an absolute deadline
// from). Adds -ETIMEDOUT.
int sem_timedwait_relative(sem_t *sem, const struct timespec *rel);

// Increments the count and wakes one waiter. Returns 0.
int sem_post(sem_t *sem);

// Stores the current count. Returns 0, or -EINVAL.
int sem_getvalue(sem_t *sem, int *value);

#endif
