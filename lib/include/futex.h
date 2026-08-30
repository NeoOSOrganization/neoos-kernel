#ifndef NEOOS_FUTEX_H
#define NEOOS_FUTEX_H

// The raw futex, exposed rather than hidden.
//
// <semaphore.h> and <pthread.h> are both built on it, and anything else
// that needs to block on a word of memory should use it too rather than
// inventing a second mechanism. The point of a futex is that the
// uncontended path never enters the kernel: a lock or an unlock is one
// compare-exchange in userland, and these calls run only when that
// fails.
//
// Operation numbers, argument order and return values are Linux's,
// unchanged -- see docs/stdlib.md for what is implemented and what is
// not. NeoOS's syscall NUMBER differs, which is the shim's business,
// not the caller's.

struct timespec;

#define FUTEX_WAIT           0
#define FUTEX_WAKE           1
#define FUTEX_PRIVATE_FLAG 128

// The full call, for anything the two helpers below do not cover.
long futex(int *uaddr, int op, int val, const struct timespec *timeout);

// Sleeps while *uaddr == expected. The comparison happens inside the
// kernel, under the same lock a wake must take, which is the whole
// point: a value change published between your own read and this call
// cannot lose the wake.
//
// Returns 0 if woken, -EAGAIN if the value had already changed, or
// -EINTR. A return of 0 does NOT prove the condition you care about
// holds -- futex wakeups are permitted to be spurious, so every caller
// re-checks in a loop.
int futex_wait(int *uaddr, int expected);

// As futex_wait, with a RELATIVE timeout; adds -ETIMEDOUT.
int futex_wait_timeout(int *uaddr, int expected, const struct timespec *rel);

// Wakes up to `count` waiters, returning how many were woken. Waking a
// futex nobody is waiting on is a no-op returning 0, not an error --
// every uncontended unlock does exactly that.
int futex_wake(int *uaddr, int count);

#endif
