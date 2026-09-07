#ifndef NEOOS_FUTEX_H
#define NEOOS_FUTEX_H

#include <stdint.h>

// Linux's futex, with Linux's operation numbers and Linux's semantics.
// The syscall NUMBER is NeoOS's own (see syscall_nr.h); everything
// behind it is Linux-shaped, because musl's pthread mutexes, condition
// variables, semaphores, barriers and once-control are all built
// directly on these two operations and cannot be shimmed into a
// different shape.
//
// Only WAIT and WAKE are implemented. See docs/stdlib.md for what that
// leaves out and what it costs.
#define FUTEX_WAIT            0
#define FUTEX_WAKE            1
#define FUTEX_REQUEUE         3
#define FUTEX_CMP_REQUEUE     4

// Accepted and IGNORED. Linux uses it to skip the shared-key lookup for
// futexes it knows are process-private; NeoOS keys every futex by
// physical address, which is correct for both cases, so the hint has
// nothing to change. Accepting it matters: musl sets it on almost every
// futex call, and rejecting an unknown flag would break all of them.
#define FUTEX_PRIVATE_FLAG  128
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_CMD_MASK      (~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME))

struct k_timespec;

// FUTEX_WAIT: sleeps if *uaddr == val. Returns 0 if woken by a
//   FUTEX_WAKE, -EAGAIN if the value did not match (checked atomically
//   against the wake), -ETIMEDOUT, or -EINTR.
// FUTEX_WAKE: wakes up to `val` waiters and returns how many it woke.
// FUTEX_REQUEUE / FUTEX_CMP_REQUEUE: see the DIVERGENCE note on
//   futex_requeue in futex.c -- NeoOS WAKES the waiters it is asked to
//   move instead of moving them, which is a legal (if less efficient)
//   implementation because every futex user must already tolerate a
//   spurious wakeup. CMP_REQUEUE checks *uaddr == val3 first and
//   returns -EAGAIN if it does not match.
// Anything else: -ENOSYS.
//
// `timeout` is RELATIVE, as Linux's FUTEX_WAIT is, and may be null.
// For the REQUEUE operations that same argument slot is Linux's `val2`
// (the count to requeue), not a pointer -- which is why it is passed
// separately rather than cast.
int64_t futex_op(uint32_t *uaddr, int op, uint32_t val,
                 const struct k_timespec *timeout,
                 uint32_t val2, uint32_t val3);

void futex_init(void);
void futex_selftest(void);

#endif
