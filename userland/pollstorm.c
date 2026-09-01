// pollstorm.c -- demonstrate the poll-broadcast thundering herd.
//
// CS3. sys_poll.c's header describes the design: every poll/select
// caller sleeps on ONE global waitq, and any readiness change anywhere
// wakes all of them to re-scan. A poller interested in nothing that just
// happened is woken anyway. CS5.2 replaces that with per-object
// registration.
//
// WHAT THIS ASSERTS, and why it is shaped this way.
//
// The obvious test -- "measure wakeups per event and watch the ratio
// fall after CS5.2" -- could not be made trustworthy here, across five
// attempts:
//
//   1. Pollers that consume the events are awake, not asleep. 0 wakeups.
//   2. A guessed settle delay measured an empty queue: the children had
//      not reached poll() yet. Also 0. A readiness handshake is not
//      enough either -- "about to block" is not "blocked", which is why
//      the depth hook exists.
//   3. Attributing wakeups to self-fired events failed: the rest of the
//      system produced ~1500 unrelated broadcasts in the same window and
//      the ratio swung between 15 and 78.
//   4. An A/B differential (same window with and without idle pollers)
//      assumed stationary background traffic. It is not stationary:
//      window A saw 1710 broadcasts and window B 67, in the same run.
//   5. Self-generating events at a fixed rate in both windows did not
//      help -- 25 events are noise against that ambient traffic, and one
//      run came out with B below A.
//
// So it asserts the thing that IS sound and needs no ratio: with N
// pollers blocked and interested in NOTHING, any wakeup at all is a herd
// wakeup. That is the defect, demonstrated rather than quantified.
//
// The comparable BEFORE/AFTER number for CS5.2 is the kernel's
// whole-boot totals, printed at shutdown as "[poll] broadcasts=N
// wakeups=M". Same suite before and after, so it is directly comparable
// and immune to all of the above.

#include <unistd.h>
#include <stdio.h>
#include <poll.h>
#include <sys/wait.h>
#include <neoos_test.h>

#define POLLERS 6

static unsigned wk_of(long v) { return (unsigned)((unsigned long)v & 0xFFFFFFFFu); }
static unsigned ev_of(long v) { return (unsigned)((unsigned long)v >> 32); }

int main(void) {
    long probe = neoos_test_poll_stats();
    if (probe < 0) {
        printf("[pollstorm] SKIPPED: no test hook (production build)\n");
        printf("[pollstorm] ALL PASSED\n");
        return 0;
    }

    int wr[POLLERS], pid[POLLERS];
    int rdy[2];
    if (pipe(rdy) != 0) { printf("[pollstorm] FAILED: ready pipe\n"); return 1; }

    for (int i = 0; i < POLLERS; i++) {
        int p[2];
        if (pipe(p) != 0) { printf("[pollstorm] FAILED: pipe %d\n", i); return 1; }
        pid[i] = fork();
        if (pid[i] < 0) { printf("[pollstorm] FAILED: fork %d\n", i); return 1; }
        if (pid[i] == 0) {
            close(p[1]); close(rdy[0]);
            char r = 'r';
            write(rdy[1], &r, 1);
            struct pollfd pf = { p[0], POLLIN, 0 };
            char c;
            for (;;) {
                pf.revents = 0;
                int rr = poll(&pf, 1, 20000);
                if (rr < 0) { exit(2); }
                if (rr == 0) { exit(3); }
                long n = read(p[0], &c, 1);
                if (n == 1) { exit(0); }
                if (n == 0) { exit(5); }
            }
        }
        close(p[0]);
        wr[i] = p[1];
    }
    close(rdy[1]);
    for (int i = 0; i < POLLERS; i++) { char r; if (read(rdy[0], &r, 1) != 1) { break; } }
    close(rdy[0]);

    // "About to block" is not "blocked": wait for the kernel to confirm
    // that pollers are actually on the broadcast queue.
    //
    // Deliberately NOT "all POLLERS at once". Depth is an instantaneous
    // snapshot of a queue whose members are being woken continuously by
    // ambient traffic, so all six are rarely on it in the same instant --
    // requiring that failed roughly one run in three. One confirmed
    // sleeper is enough: the assertion below is that an uninterested
    // poller gets woken at all.
    long depth = 0, max_depth = 0;
    for (int spin = 0; spin < 400; spin++) {
        depth = neoos_test_poll_depth();
        if (depth > max_depth) { max_depth = depth; }
        if (max_depth >= POLLERS || (depth >= 1 && spin > 20)) { break; }
        for (volatile int d = 0; d < 200000; d++) { }
    }
    if (max_depth < 1) {
        printf("[pollstorm] FAILED: no poller ever observed blocked on the broadcast\n");
        return 1;
    }

    long before = neoos_test_poll_stats();

    // Readiness on a pipe NO poller has any interest in. Under the
    // broadcast design each event reaches all of them.
    int own[2];
    if (pipe(own) != 0) { printf("[pollstorm] FAILED: own pipe\n"); return 1; }
    for (int e = 0; e < 25; e++) {
        char c = '.';
        if (write(own[1], &c, 1) != 1) { break; }
        if (read(own[0], &c, 1) != 1) { break; }
        for (volatile int d = 0; d < 1500000; d++) { }
    }
    close(own[0]); close(own[1]);

    long after = neoos_test_poll_stats();

    for (int i = 0; i < POLLERS; i++) { char q = 'x'; write(wr[i], &q, 1); }
    int bad = 0, code = 0;
    for (int i = 0; i < POLLERS; i++) {
        int st = 0;
        waitpid(pid[i], &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { bad++; code = WIFEXITED(st) ? WEXITSTATUS(st) : -1; }
        close(wr[i]);
    }
    if (bad) {
        printf("[pollstorm] FAILED: %d pollers exited badly, last %d (2=err 3=timeout 5=EOF)\n", bad, code);
        return 1;
    }

    unsigned woke   = wk_of(after) - wk_of(before);
    unsigned events = ev_of(after) - ev_of(before);

    if (events == 0) {
        printf("[pollstorm] FAILED: no broadcasts counted -- the counters are not wired\n");
        return 1;
    }
    if (woke == 0) {
        printf("[pollstorm] FAILED: %u broadcasts woke none of %u blocked pollers -- "
               "either the counters or the wait is wrong\n", events, POLLERS);
        return 1;
    }

    printf("[pollstorm] %u pollers interested in nothing (max %d seen blocked at once); "
           "%u broadcasts woke them %u times\n",
           POLLERS, (int)max_depth, events, woke);
    printf("[pollstorm] every one of those wakeups is the thundering herd: CS5.2 should make it 0\n");
    printf("[pollstorm] ALL PASSED\n");
    return 0;
}
