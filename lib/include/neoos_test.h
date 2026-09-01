#ifndef NEOOS_TEST_H
#define NEOOS_TEST_H

// Test-only wrappers for the SYS_TEST_HOOK syscall. These are
// conditionally compiled and return -ENOSYS in production builds.

// Inject a key event as if it came from the keyboard. Compiled only
// in test builds; returns -ENOSYS in production.
int neoos_test_inject_key(unsigned keycode, int pressed);

// Read the user-thread migration counter. Used to verify that the
// kernel steal path was exercised. Returns -ENOSYS in production.
long neoos_test_migration_count(void);

// Return the parent_pid of process `pid`, or -ESRCH if no such process
// (including one already reaped). Returns -ENOSYS in production.
int neoos_test_parent_pid(int pid);

// Free physical frames, for tests that assert memory comes back after a
// stress loop. Returns -ENOSYS in production.
long neoos_test_pmm_free(void);

// Poll-broadcast statistics packed as (events << 32) | wakeups, for the
// thundering-herd baseline. Returns -ENOSYS in production.
long neoos_test_poll_stats(void);

#endif
