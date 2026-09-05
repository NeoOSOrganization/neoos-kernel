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

// Threads blocked on the poll broadcast right now. Returns -ENOSYS in
// production.
long neoos_test_poll_depth(void);

// Poll wakeups that found nothing ready -- the wasted ones. Returns
// -ENOSYS in production.
long neoos_test_poll_wasted(void);

// D5's fault injector: drop one transmitted TCP segment in `drop_1_in_n`
// and reorder one in `reorder_1_in_m`; zero disables. Loopback and slirp
// both deliver perfectly, so this is the only way the retransmission and
// reassembly paths ever execute. Returns -ENOSYS in production.
int  neoos_test_tcp_fault(unsigned drop_1_in_n, unsigned reorder_1_in_m);
long neoos_test_tcp_retrans(void);
long neoos_test_tcp_reasm(void);

#endif
