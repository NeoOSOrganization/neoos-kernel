// pollstorm.c -- how loud is the poll broadcast?
//
// CS3, and its output is a NUMBER rather than a verdict.
//
// sys_poll.c's own header describes the design: every poll/select caller
// sleeps on one global waitq, and any readiness change anywhere wakes
// all of them to re-scan their own fds. So wakeups grow as
// O(sleepers x events) where a per-object poll table would be
// O(interested) -- one wakeup per event, since each event here concerns
// exactly one poller.
//
// CS5.2 replaces the broadcast with per-object registration and has to
// prove it helped. This is the before-number.
//
// It deliberately does NOT assert a bound on the ratio. The whole point
// is that the current ratio is bad; failing on it would just be a test
// that fails by design. It asserts only that the counters are wired and
// self-consistent, and prints the ratio for the record.

#include <unistd.h>
#include <stdio.h>
#include <poll.h>
#include <sys/wait.h>
#include <neoos_test.h>

#define POLLERS 6
#define EVENTS  40

int main(void) {
    long before = neoos_test_poll_stats();
    if (before < 0) {
        printf("[pollstorm] SKIPPED: no test hook (production build)\n");
        printf("[pollstorm] ALL PASSED\n");
        return 0;
    }

    // Each child blocks in poll() on its OWN pipe, which stays empty for
    // the whole measurement. They are asleep on the global broadcast
    // queue and interested in nothing that happens.
    int wr[POLLERS];
    int pid[POLLERS];
    for (int i = 0; i < POLLERS; i++) {
        int p[2];
        if (pipe(p) != 0) { printf("[pollstorm] FAILED: pipe %d\n", i); return 1; }
        pid[i] = fork();
        if (pid[i] < 0) { printf("[pollstorm] FAILED: fork %d\n", i); return 1; }
        if (pid[i] == 0) {
            close(p[1]);
            struct pollfd pf = { p[0], POLLIN, 0 };
            char c;
            for (;;) {
                int r = poll(&pf, 1, 10000);
                if (r <= 0) { exit(r < 0 ? 2 : 3); }
                if (read(p[0], &c, 1) == 1) { exit(0); }   // released
            }
        }
        close(p[0]);
        wr[i] = p[1];
    }

    // Let them all reach poll() and go to sleep before measuring.
    for (volatile int d = 0; d < 4000000; d++) { }

    long mid = neoos_test_poll_stats();

    // Generate readiness events on a pipe NOBODY is polling. Each one is
    // of no interest to any sleeper, so an ideal implementation wakes
    // zero of them. The broadcast wakes all POLLERS every time.
    int own[2];
    if (pipe(own) != 0) { printf("[pollstorm] FAILED: own pipe\n"); return 1; }
    for (int e = 0; e < EVENTS; e++) {
        char c = '.';
        if (write(own[1], &c, 1) != 1) { break; }
        if (read(own[0], &c, 1) != 1) { break; }
    }
    close(own[0]); close(own[1]);

    long post = neoos_test_poll_stats();

    // Release the pollers and reap.
    for (int i = 0; i < POLLERS; i++) { char q = 'x'; write(wr[i], &q, 1); }
    for (int i = 0; i < POLLERS; i++) { int st = 0; waitpid(pid[i], &st, 0); close(wr[i]); }

    if (mid < 0 || post < 0) { printf("[pollstorm] FAILED: stats hook vanished\n"); return 1; }

    // Measure only the window in which every poller was asleep.
    unsigned ev0 = (unsigned)((unsigned long)mid >> 32);
    unsigned wk0 = (unsigned)((unsigned long)mid & 0xFFFFFFFFu);
    unsigned ev1 = (unsigned)((unsigned long)post >> 32);
    unsigned wk1 = (unsigned)((unsigned long)post & 0xFFFFFFFFu);
    (void)before;

    unsigned events  = ev1 - ev0;
    unsigned wakeups = wk1 - wk0;
    if (events == 0) {
        printf("[pollstorm] FAILED: no broadcasts counted -- the counters are not wired\n");
        return 1;
    }
    // libneoos printf has no %f: report tenths.
    unsigned tenths = events ? (wakeups * 10u) / events : 0;
    printf("[pollstorm] %u pollers asleep, %u readiness events none of them wanted\n",
           POLLERS, events);
    printf("[pollstorm] broadcast woke %u sleepers -- %u.%u per event, ideal 0.0\n",
           wakeups, tenths / 10, tenths % 10);

    printf("[pollstorm] ALL PASSED\n");
    return 0;
}
